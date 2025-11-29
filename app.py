# app.py - Fully integrated Streamlit app (server-side OSRM, robust I/O, history)
import streamlit as st
import os
import math
import json
import uuid
import requests
from streamlit.components.v1 import html
from datetime import datetime

# ------------------------------
# Safe I/O helpers (utf-8)
# ------------------------------
def safe_read_lines(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return [ln.rstrip("\n") for ln in f]
    except FileNotFoundError:
        return []
    except Exception:
        try:
            with open(path, "r", errors="replace") as f:
                return [ln.rstrip("\n") for ln in f]
        except Exception:
            return []

def safe_write_all(path, lines):
    try:
        with open(path, "w", encoding="utf-8", errors="replace") as f:
            for ln in lines:
                f.write(ln + "\n")
        return True, None
    except Exception as e:
        try:
            with open(path, "wb") as f:
                data = ("\n".join(lines) + "\n").encode("utf-8", errors="replace")
                f.write(data)
            return True, None
        except Exception as e2:
            return False, str(e2)

def safe_append_history(path, line):
    try:
        with open(path, "a", encoding="utf-8", errors="replace") as f:
            f.write(line + "\n")
        return True, None
    except Exception as e:
        try:
            with open(path, "ab") as f:
                f.write((line + "\n").encode("utf-8", errors="replace"))
            return True, None
        except Exception as e2:
            return False, f"{e}  (fallback failed: {e2})"

# ------------------------------
# Data loaders (utf-8)
# ------------------------------
def load_places(filename="places.txt"):
    places = {}
    if not os.path.exists(filename):
        return places
    with open(filename, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) == 3:
                name, lat, lon = parts
                try:
                    places[name.lower()] = {"lat": float(lat), "lon": float(lon)}
                except:
                    pass
    return places

def load_cities(filename="cities.txt"):
    cities = {}
    if not os.path.exists(filename):
        return cities
    with open(filename, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(",")
            if len(parts) == 3:
                name, lon, lat = parts
                try:
                    cities[name.strip()] = {"lat": float(lat), "lon": float(lon)}
                except:
                    pass
    return cities

def load_cars(filename="cars.txt"):
    cars = {}
    if not os.path.exists(filename):
        cars["default"] = 120.0
        return cars
    with open(filename, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "," in line:
                model, co2 = line.strip().split(",", 1)
                model = model.lower().strip()
                try:
                    cars[model] = float(co2)
                except:
                    pass
    if "default" not in cars:
        cars["default"] = 120.0
    return cars

# ------------------------------
# Utilities
# ------------------------------
def haversine(lat1, lon1, lat2, lon2):
    R = 6371
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlon / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))
    return R * c

# ------------------------------
# Polyline decoder (precision 5)
# ------------------------------
def decode_polyline(encoded, precision=5):
    coords = []
    index = 0
    lat = 0
    lng = 0
    factor = 10 ** precision

    while index < len(encoded):
        shift = 0
        result = 0
        while True:
            b = ord(encoded[index]) - 63
            index += 1
            result |= (b & 0x1f) << shift
            shift += 5
            if b < 0x20:
                break
        dlat = ~(result >> 1) if (result & 1) else (result >> 1)
        lat += dlat

        shift = 0
        result = 0
        while True:
            b = ord(encoded[index]) - 63
            index += 1
            result |= (b & 0x1f) << shift
            shift += 5
            if b < 0x20:
                break
        dlng = ~(result >> 1) if (result & 1) else (result >> 1)
        lng += dlng

        coords.append([lat / factor, lng / factor])
    return coords

# ------------------------------
# OSRM base URL
# ------------------------------
OSRM_BASE = "https://router.project-osrm.org"

# ------------------------------
# Robust get_osrm_routes with multiple fallbacks and debug expander
# ------------------------------
def get_osrm_routes(start, end, profiles=("driving", "cycling", "walking")):
    """
    Server-side OSRM fetch with:
    - polyline -> geojson fallbacks
    - retry with swapped lat/lon ordering if nothing found
    - final fallback attempting lat,lon URL ordering
    - debug expander in Streamlit showing URLs/snippets
    Returns list of route dicts: {profile, profileName, idx, distance, duration, coords:[[lat,lon],...]}
    """
    results = []

    def try_url(url):
        try:
            resp = requests.get(url, timeout=10)
        except Exception as e:
            return None, {"error": str(e)}
        try:
            j = resp.json()
            return j, {"status_code": resp.status_code, "json_keys": list(j.keys())[:8]}
        except Exception:
            return None, {"status_code": resp.status_code, "text_head": resp.text[:800]}

    # debug expander
    try:
        dbg = st.expander("OSRM debug (click to expand)", expanded=True)
        dbg.write("Querying OSRM server (server-side). Will try polyline then geojson; auto-retry with swapped coords if needed.")
    except Exception:
        dbg = None

    def attempt_profiles(coord_start, coord_end, label):
        local_results = []
        for prof in profiles:
            # 1) polyline
            url_poly = f"{OSRM_BASE}/route/v1/{prof}/{coord_start['lon']},{coord_start['lat']};{coord_end['lon']},{coord_end['lat']}?overview=full&geometries=polyline&alternatives=true"
            j_poly, snip_poly = try_url(url_poly)
            if dbg:
                with dbg:
                    st.write(f"[{label}] PROFILE: {prof} (polyline)")
                    st.write("URL:", url_poly)
                    st.write("Snippet:", snip_poly)
            if j_poly and j_poly.get("routes"):
                for i, route in enumerate(j_poly["routes"]):
                    geom = route.get("geometry")
                    if not geom:
                        continue
                    coords = decode_polyline(geom, precision=5)
                    local_results.append({
                        "profile": prof,
                        "profileName": prof.title(),
                        "idx": i,
                        "distance": route.get("distance", 0.0),
                        "duration": route.get("duration", 0.0),
                        "coords": coords
                    })
                continue

            # 2) geojson
            url_geo = f"{OSRM_BASE}/route/v1/{prof}/{coord_start['lon']},{coord_start['lat']};{coord_end['lon']},{coord_end['lat']}?overview=full&geometries=geojson&alternatives=true"
            j_geo, snip_geo = try_url(url_geo)
            if dbg:
                with dbg:
                    st.write(f"[{label}] PROFILE: {prof} (geojson)")
                    st.write("URL:", url_geo)
                    st.write("Snippet:", snip_geo)
            if j_geo and j_geo.get("routes"):
                for i, route in enumerate(j_geo["routes"]):
                    geom = route.get("geometry")
                    coords = []
                    coords_raw = geom.get("coordinates") if isinstance(geom, dict) else None
                    if coords_raw:
                        coords = [[pt[1], pt[0]] for pt in coords_raw]
                    local_results.append({
                        "profile": prof,
                        "profileName": prof.title(),
                        "idx": i,
                        "distance": route.get("distance", 0.0),
                        "duration": route.get("duration", 0.0),
                        "coords": coords
                    })
        return local_results

    # Attempt 1: as-provided
    if dbg:
        with dbg:
            st.write("Attempt 1: using coordinates as provided.")
    res1 = attempt_profiles(start, end, label="as-provided")
    results.extend(res1)

    # Attempt 2: swapped lat/lon (handles swapped input files)
    if not results:
        swapped_start = {"lat": start["lon"], "lon": start["lat"]}
        swapped_end   = {"lat": end["lon"], "lon": end["lat"]}
        if dbg:
            with dbg:
                st.write("No routes found with provided ordering. Attempt 2: trying swapped lat/lon coordinates.")
                st.write("Swapped start:", swapped_start, "Swapped end:", swapped_end)
        res2 = attempt_profiles(swapped_start, swapped_end, label="swapped")
        results.extend(res2)

    # Attempt 3: last-resort vary URL ordering (lat,lon in URL)
    if not results:
        if dbg:
            with dbg:
                st.write("Attempt 3: trying URL with lat,lon ordering as a last-resort.")
        def attempt_url_latlon(coord_start, coord_end):
            fallback_results = []
            for prof in profiles:
                url_poly = f"{OSRM_BASE}/route/v1/{prof}/{coord_start['lat']},{coord_start['lon']};{coord_end['lat']},{coord_end['lon']}?overview=full&geometries=polyline&alternatives=true"
                j_poly, snip_poly = try_url(url_poly)
                if dbg:
                    with dbg:
                        st.write(f"[url-latlon] PROFILE: {prof} (polyline)")
                        st.write("URL:", url_poly)
                        st.write("Snippet:", snip_poly)
                if j_poly and j_poly.get("routes"):
                    for i, route in enumerate(j_poly["routes"]):
                        geom = route.get("geometry")
                        if not geom:
                            continue
                        coords = decode_polyline(geom, precision=5)
                        fallback_results.append({
                            "profile": prof,
                            "profileName": prof.title(),
                            "idx": i,
                            "distance": route.get("distance", 0.0),
                            "duration": route.get("duration", 0.0),
                            "coords": coords
                        })
                    continue
                url_geo = f"{OSRM_BASE}/route/v1/{prof}/{coord_start['lat']},{coord_start['lon']};{coord_end['lat']},{coord_end['lon']}?overview=full&geometries=geojson&alternatives=true"
                j_geo, snip_geo = try_url(url_geo)
                if dbg:
                    with dbg:
                        st.write(f"[url-latlon] PROFILE: {prof} (geojson)")
                        st.write("URL:", url_geo)
                        st.write("Snippet:", snip_geo)
                if j_geo and j_geo.get("routes"):
                    for i, route in enumerate(j_geo["routes"]):
                        geom = route.get("geometry")
                        coords = []
                        coords_raw = geom.get("coordinates") if isinstance(geom, dict) else None
                        if coords_raw:
                            coords = [[pt[1], pt[0]] for pt in coords_raw]
                        fallback_results.append({
                            "profile": prof,
                            "profileName": prof.title(),
                            "idx": i,
                            "distance": route.get("distance", 0.0),
                            "duration": route.get("duration", 0.0),
                            "coords": coords
                        })
            return fallback_results
        res3 = attempt_url_latlon(start, end)
        results.extend(res3)

    if dbg:
        with dbg:
            st.write("Final total routes found:", len(results))
            for r in results[:8]:
                st.write(f"{r['profileName']} idx={r['idx']} — {r['distance']/1000:.2f} km, {r['duration']/60:.0f} min, pts={len(r['coords'])}")

    return results

# ------------------------------
# Build HTML with inlined routes (no client OSRM calls)
# ------------------------------
def build_route_map_html_inlined(start, end, names, routes, default_color="#0066FF", map_id=None):
    if map_id is None:
        map_id = "map_" + uuid.uuid4().hex[:8]
    n0 = names[0].replace('"', '\\"')
    n1 = names[1].replace('"', '\\"')

    safe_routes = []
    for r in routes:
        # ensure coords are list of [lat,lon] floats
        safe_coords = [[float(pt[0]), float(pt[1])] for pt in r["coords"]]
        safe_routes.append({
            "profile": r.get("profile"),
            "profileName": r.get("profileName"),
            "idx": r.get("idx"),
            "distance": r.get("distance"),
            "duration": r.get("duration"),
            "coords": safe_coords
        })
    routes_json = json.dumps(safe_routes)

    template = f"""<!doctype html>
<html>
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
  body{{margin:0;padding:0;font-family:system-ui,Segoe UI,Roboto,Arial}}
  #container{{display:flex;flex-direction:column;height:100vh}}
  #map{{flex:1}}
  #controls{{padding:8px;background:#fff;box-shadow:0 1px 8px rgba(0,0,0,0.08);z-index:999}}
  .route-info{{margin-right:12px;display:inline-block;vertical-align:middle}}
  .alt-route{{cursor:pointer;padding:4px 8px;border-radius:6px;margin:4px;display:inline-block;background:#f1f1f1}}
  .alt-route.selected{{background:{default_color};color:white}}
  #topbar{{display:flex;align-items:center;gap:12px}}
  .small{{font-size:0.9em;color:#333}}
  button{{padding:6px 10px;border-radius:6px;border:0;background:{default_color};color:white}}
  input[type=range]{{width:120px}}
</style>
</head>
<body>
<div id="container">
  <div id="controls">
    <div id="topbar">
      <div class="route-info"><strong>{n0}</strong> -> <strong>{n1}</strong></div>
      <div id="modeTimes" class="small">Loading times…</div>
      <div style="flex:1"></div>
      <div>
        <button id="playBtn">Play</button>
        <button id="pauseBtn" style="display:none">Pause</button>
      </div>
      <div style="margin-left:8px">speed: <input type="range" id="speed" min="0.25" max="5" step="0.25" value="1"></div>
    </div>
    <div id="alts" style="margin-top:8px" class="small">Alternate routes will appear here</div>
  </div>

  <div id="{map_id}" style="height:100%"></div>
</div>

<script>
var map = L.map('{map_id}').setView([{start['lat']}, {start['lon']}], 15);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {{maxZoom:19}}).addTo(map);

L.marker([{start['lat']},{start['lon']}]).addTo(map).bindPopup("{n0}");
L.marker([{end['lat']},{end['lon']}]).addTo(map).bindPopup("{n1}");

var routeData = {routes_json};
var polylines = [];
var allRoutes = routeData || [];
var selectedIndex = 0;
var altColors = ['#0066FF','#00AA66','#FF7700','#AA00CC','#444444'];

function formatDuration(sec) {{
    if (sec == null) return '-';
    sec = Math.round(sec);
    if (sec < 60) return sec + ' s';
    let mins = Math.floor(sec / 60);
    if (mins < 60) return mins + ' min';
    let hours = Math.floor(mins / 60);
    mins = mins % 60;
    return hours + ' h ' + mins + ' m';
}}

function drawRoutes() {{
    polylines.forEach(p=>map.removeLayer(p));
    polylines = [];
    var altDiv = document.getElementById('alts');
    altDiv.innerHTML = '';
    for (var i=0;i<allRoutes.length;i++) {{
        var r = allRoutes[i];
        var color = altColors[i % altColors.length];
        var pl = L.polyline(r.coords, {{color: color, weight:6, opacity:0.85}}).addTo(map);
        r.polylineObj = pl;
        polylines.push(pl);
        var el = document.createElement('span');
        el.className = 'alt-route';
        el.dataset.idx = i;
        el.style.border = '1px solid rgba(0,0,0,0.06)';
        el.innerText = r.profileName + ' #' + (r.idx+1) + ' — ' + (r.distance/1000).toFixed(2) + ' km / ' + formatDuration(r.duration);
        el.onclick = function() {{ selectRoute(parseInt(this.dataset.idx)); }};
        altDiv.appendChild(el);
    }}
    if (polylines.length>0) {{
        map.fitBounds(polylines[0].getBounds().pad(0.1));
        selectRoute(0);
    }} else {{
        var coords = [[{start['lat']},{start['lon']}],[{end['lat']},{end['lon']}]];
        var pl = L.polyline(coords, {{color: '{default_color}', weight:6}}).addTo(map);
        polylines.push(pl);
        map.fitBounds(pl.getBounds().pad(0.1));
        document.getElementById('modeTimes').innerText = 'No OSRM routes';
    }}
}}

function clearSelectedUI() {{ document.querySelectorAll('.alt-route').forEach(x=>x.classList.remove('selected')); }}
function selectRoute(idx) {{
    if (!allRoutes[idx]) return;
    selectedIndex = idx;
    for (var i=0;i<allRoutes.length;i++) {{
        var p = allRoutes[i].polylineObj;
        if (i===idx) p.setStyle({{weight:8, opacity:1}});
        else p.setStyle({{weight:6, opacity:0.4}});
    }}
    clearSelectedUI();
    var elems = document.querySelectorAll('.alt-route');
    if (elems[idx]) elems[idx].classList.add('selected');
    var r = allRoutes[idx];
    document.getElementById('modeTimes').innerText = r.profileName + ' selected — ' + (r.distance/1000).toFixed(2) + ' km / ' + formatDuration(r.duration);
    if (movingMarker) movingMarker.setLatLng(r.coords[0]);
}}

var movingMarker = null;
var animHandle = null;
var animPos = 0;
var animRouteCoords = [];
var animSpeed = 1.0;

function startAnimation() {{
    var r = allRoutes[selectedIndex];
    if (!r) return;
    animRouteCoords = r.coords.slice();
    if (animRouteCoords.length===0) return;
    if (!movingMarker) movingMarker = L.marker(animRouteCoords[0], {{icon: L.icon({{iconUrl:'https://cdn-icons-png.flaticon.com/512/252/252025.png', iconSize:[32,32]}})}}).addTo(map);
    animPos = 0;
    cancelAnimationFrame(animHandle);
    function step() {{
        var stepSize = Math.max(1, Math.round(animSpeed * 1));
        animPos += stepSize;
        if (animPos >= animRouteCoords.length) {{
            movingMarker.setLatLng(animRouteCoords[animRouteCoords.length-1]);
            cancelAnimationFrame(animHandle);
            document.getElementById('playBtn').style.display='inline-block';
            document.getElementById('pauseBtn').style.display='none';
            return;
        }}
        movingMarker.setLatLng(animRouteCoords[animPos]);
        animHandle = requestAnimationFrame(step);
    }}
    animHandle = requestAnimationFrame(step);
}}
function pauseAnimation() {{ if (animHandle) cancelAnimationFrame(animHandle); }}

document.getElementById('playBtn').onclick = function() {{
    animSpeed = parseFloat(document.getElementById('speed').value);
    startAnimation();
    document.getElementById('playBtn').style.display='none';
    document.getElementById('pauseBtn').style.display='inline-block';
}};
document.getElementById('pauseBtn').onclick = function() {{
    pauseAnimation();
    document.getElementById('playBtn').style.display='inline-block';
    document.getElementById('pauseBtn').style.display='none';
}};
document.getElementById('speed').oninput = function() {{ animSpeed = parseFloat(this.value); }};

drawRoutes();
</script>
</body>
</html>
"""
    return template

# ------------------------------
# Build offline HTML (markers only)
# ------------------------------
def build_offline_html(start, end, names):
    map_id = "offline_" + uuid.uuid4().hex[:8]
    n0 = names[0].replace('"','\\"'); n1 = names[1].replace('"','\\"')
    html_doc = f"""<!doctype html>
<html><head><meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css"/>
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>html,body,#map{{height:100%;margin:0;padding:0}}</style></head><body>
<div id="{map_id}" style="height:100vh"></div>
<script>var map = L.map('{map_id}').setView([{start['lat']},{start['lon']}],13); L.marker([{start['lat']},{start['lon']}]).addTo(map).bindPopup("{n0}"); L.marker([{end['lat']},{end['lon']}]).addTo(map).bindPopup("{n1}");</script>
</body></html>"""
    return html_doc

# ------------------------------
# Streamlit UI
# ------------------------------
st.set_page_config(page_title="EcoRoute", layout="wide")
st.title("🌍 EcoRoute Navigation (server-side OSRM)")

places = load_places()
cities = load_cities()
cars = load_cars()

if "logged_in" not in st.session_state: st.session_state.logged_in = False
if "username" not in st.session_state: st.session_state.username = None

# Auth UI
st.sidebar.title("User Access")
mode = st.sidebar.radio("Choose:", ["Login", "Register"])
if "show_pass" not in st.session_state: st.session_state.show_pass = False
def pass_input(label, key):
    if st.session_state.show_pass: return st.sidebar.text_input(label, key=key)
    return st.sidebar.text_input(label, type="password", key=key)
if st.sidebar.checkbox("👁️ Show password"): st.session_state.show_pass = True
else: st.session_state.show_pass = False

# Login
if mode == "Login":
    uname = st.sidebar.text_input("Username")
    pwd = pass_input("Password", "pwd_in")
    if st.sidebar.button("Login"):
        users = safe_read_lines("up.txt")
        ok = False
        for line in users:
            parts = line.split()
            if len(parts) >= 2 and parts[0] == uname and parts[1] == pwd:
                ok = True; break
        if ok:
            st.session_state.logged_in = True
            st.session_state.username = uname
            st.success(f"Welcome back, {uname}! 🎉")
        else:
            st.error("❌ Invalid username or password")

# Register
elif mode == "Register":
    newu = st.sidebar.text_input("Create username")
    newp = pass_input("Create password", "regpass")
    if st.sidebar.button("Register"):
        if len(newu.strip()) < 3: st.error("⚠️ Username must be at least 3 characters")
        elif len(newp) < 6: st.error("⚠️ Password must be at least 6 characters")
        else:
            users = safe_read_lines("up.txt")
            exists = any((line.split()[0] == newu) for line in users if line.strip())
            if exists: st.error("⚠️ Username already exists")
            else:
                try:
                    with open("up.txt", "a", encoding="utf-8", errors="replace") as f:
                        f.write(newu + " " + newp + "\n")
                    st.success("🎉 Registered successfully! Please login.")
                except Exception as e:
                    st.error("Failed to register: " + str(e))

# After login
if st.session_state.logged_in:
    st.markdown("---")
    st.subheader(f"Welcome **{st.session_state.username}** 👋")
    option = st.radio("📦 Choose Module", ["Shortest Route", "Carbon Route", "Route History"], horizontal=True)

    safe_user = "".join(ch for ch in st.session_state.username if ch.isalnum() or ch in ("_", "-")).strip()
    if not safe_user: safe_user = "user"
    history_file = f"{safe_user}_history.txt"

    # Shortest Route
    if option == "Shortest Route":
        st.header("🗺️ Local Navigation (server-side OSRM alternates + animation)")
        place_keys = [p.title() for p in places.keys()]
        if not place_keys:
            st.warning("No places loaded (places.txt missing). Example format: name lat lon (latitude first).")
        else:
            start = st.selectbox("Start", place_keys, index=0)
            end = st.selectbox("End", place_keys, index=min(1, len(place_keys)-1))
            mode2 = st.selectbox("Routing preference (UI color)", ["🚗 Standard", "🚶 Walk optimized", "🛵 Two-wheeler"])
            color_map = {"🚗 Standard":"#0066FF","🚶 Walk optimized":"#00CC66","🛵 Two-wheeler":"#FF7700"}
            if start == end:
                st.info("Choose different start and end.")
            else:
                if st.button("Compute Routes (OSRM)"):
                    s = places[start.lower()]; e = places[end.lower()]
                    st.info("Querying OSRM server (server-side)...")
                    routes = get_osrm_routes(s, e, profiles=("driving","cycling","walking"))
                    if not routes:
                        st.warning("OSRM returned no routes for these points — showing straight line fallback.")
                        offline_html = build_offline_html(s, e, [start, end])
                        html(offline_html, height=500)
                    else:
                        # display primary mode estimates
                        def pick_primary(prof):
                            g = [r for r in routes if r["profile"]==prof]
                            return g[0] if g else None
                        car = pick_primary("driving"); bike = pick_primary("cycling"); walk = pick_primary("walking")
                        st.write("Estimated travel times (primary route):")
                        st.write(f"Car: {int(car['duration']/60) if car else '-'} min, {round(car['distance']/1000,2) if car else '-'} km")
                        st.write(f"Bike: {int(bike['duration']/60) if bike else '-'} min, {round(bike['distance']/1000,2) if bike else '-'} km")
                        st.write(f"Walk: {int(walk['duration']/60) if walk else '-'} min, {round(walk['distance']/1000,2) if walk else '-'} km")
                        # show map with inlined routes
                        html_map = build_route_map_html_inlined(s, e, [start, end], routes, default_color=color_map.get(mode2,"#0066FF"))
                        html(html_map, height=700)

                    # Save history safely
                    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    arrow = " -> "
                    ok, err = safe_append_history(history_file, f"{ts} | {start}{arrow}{end} | {mode2}")
                    if not ok:
                        st.error("Failed to save history: " + (err or "unknown"))

                    # Offline download (markers only)
                    offline_html2 = build_offline_html(s, e, [start, end])
                    btn_name = f"map_{start}_{end}.html".replace(" ", "_")
                    st.download_button("⬇️ Download offline map (markers only)", offline_html2.encode("utf-8"), file_name=btn_name, mime="text/html")

    # Carbon Route
    if option == "Carbon Route":
        st.header("🌱 CO₂ Optimized City Route")
        if not cities:
            st.error("cities.txt missing or empty (expected: name,lon,lat per line)")
        else:
            src = st.selectbox("From city", list(cities.keys()))
            dst = st.selectbox("To city", list(cities.keys()))
            car_models = ["(custom)"] + [m.title() for m in sorted(cars.keys())]
            selected_model = st.selectbox("Car model (or custom)", car_models)
            if selected_model == "(custom)":
                custom_name = st.text_input("Car model name", "Custom")
                custom_co2 = st.number_input("CO₂ (g/km)", min_value=0.0, value=120.0, step=1.0)
                car_name = custom_name; co2_value = float(custom_co2)
            else:
                car_name = selected_model; key = selected_model.lower().strip(); co2_value = float(cars.get(key, cars.get("default", 120.0)))
            if src == dst:
                st.info("Select two different cities.")
            else:
                if st.button("Compute CO₂"):
                    A = cities[src]; B = cities[dst]
                    d = haversine(A["lat"], A["lon"], B["lat"], B["lon"])
                    total = d * co2_value
                    st.success(f"{src} -> {dst}")
                    st.write(f"Distance: **{d:.2f} km**")
                    st.write(f"Car model: **{car_name} ({co2_value} g/km)**")
                    st.write(f"Total CO₂ emissions: **{total:.1f} g**")
                    routes = get_osrm_routes(A, B, profiles=("driving",))
                    if routes:
                        html_map = build_route_map_html_inlined(A, B, [src, dst], routes, default_color="#444444")
                        html(html_map, height=600)
                    else:
                        html(build_offline_html(A,B,[src,dst]), height=400)
                    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                    ok, err = safe_append_history(history_file, f"{ts} | {src} -> {dst} | CO2 | {car_name} | {co2_value} g/km")
                    if not ok:
                        st.error("Failed to save history: " + (err or "unknown"))

    # Route History
    if option == "Route History":
        st.header("📜 Your Saved Routes")
        lines = safe_read_lines(history_file)
        if lines:
            st.write("Your history (oldest first). Select entries to delete:")
            to_delete = []
            for i, line in enumerate(lines):
                cols = st.columns([0.03, 0.97])
                chk = cols[0].checkbox("", key=f"del_{i}")
                cols[1].write(line)
                if chk:
                    to_delete.append(i)
            if st.button("Delete selected entries"):
                new_lines = [ln for idx, ln in enumerate(lines) if idx not in to_delete]
                ok, err = safe_write_all(history_file, new_lines)
                if ok:
                    st.success("Selected entries deleted"); st.experimental_rerun()
                else:
                    st.error("Failed to delete: " + (err or "unknown"))
            if st.button("🗑️ Clear All History"):
                try:
                    os.remove(history_file)
                    st.success("History cleared"); st.experimental_rerun()
                except Exception as e:
                    st.error("Failed to clear history: " + str(e))
        else:
            st.info("No history file found. Plan a route!")
