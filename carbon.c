/* carbon.c  -- CO2-optimized Dijkstra with TomTom hybrid caching (Option C)
   Updated: interactive map UI; JS now receives car_co2 from C.
   Input: cities.txt (CityName,Longitude,Latitude) or --places places.txt (Name LAT LON)
   Compile:
     gcc carbon.c -o carbon -lm -DUSE_TOMTOM
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
  #include <windows.h>
  #include <io.h>
  #define access _access
  #define F_OK 0
#else
  #include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------- Config -------------------- */
#define MAX_CITIES 300
#define MAX_LINE 512
#define INF 1e18
#define DEFAULT_CO2_GKM 120.0
#define SAMPLE_EVERY_N 3           /* sample every Nth undirected edge */
#define TRAFFIC_CACHE_FILE "traffic_cache.txt"
#define CACHE_TTL_MINUTES_DEFAULT 15  /* default cache TTL (minutes) */

/* Mode speeds (km/h) */
#define CAR_FREEFLOW_KMPH 50.0   /* expected free-flow car speed */
#define BIKE_KMPH 15.0
#define WALK_KMPH 5.0

/* -------------------- TomTom API key (embedded) -------------------- */
#define TOMTOM_API_KEY "c4f1baac-5522-4e4d-bb86-3c6b3370f9ec"

/* -------------------- Data structures -------------------- */

typedef struct {
    char name[128];
    double lon;
    double lat;
} City;

typedef struct {
    int v;
    double distance_km;
    double traffic_factor;
    double co2_cost; /* grams */
} Edge;

typedef struct {
    int n;
    City cities[MAX_CITIES];
    Edge *edges; /* adjacency matrix flattened: edges[i * n + j] */
} Graph;

typedef struct {
    double dist;
    int prev;
    int visited;
} DijkNode;

/* -------------------- Utilities -------------------- */

static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p)+1);
    size_t len = strlen(s);
    while (len && isspace((unsigned char)s[len-1])) s[--len] = 0;
}

static double deg2rad(double d) { return d * M_PI / 180.0; }

static double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0;
    double dlat = deg2rad(lat2 - lat1);
    double dlon = deg2rad(lon2 - lon1);
    double a = sin(dlat/2.0)*sin(dlat/2.0) +
               cos(deg2rad(lat1)) * cos(deg2rad(lat2)) * sin(dlon/2.0)*sin(dlon/2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}

/* Open a URL or file in default browser (cross-platform) */
static void open_in_browser(const char *url) {
#ifdef _WIN32
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "start \"\" \"%s\"", url);
    system(cmd);
#elif __APPLE__
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "open \"%s\" &", url);
    system(cmd);
#else
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "xdg-open \"%s\" >/dev/null 2>&1 &", url);
    system(cmd);
#endif
}

/* -------------------- Loaders -------------------- */

/* Load comma-format cities.txt: CityName,Longitude,Latitude */
int load_cities_comma(const char *fn, City *cities, int *n) {
    FILE *f = fopen(fn, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    int cnt = 0;
    while (fgets(line, sizeof(line), f) && cnt < MAX_CITIES) {
        line[strcspn(line, "\n")] = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        trim(line);
        if (strlen(line) == 0) continue;
        char name[128]; double lon, lat;
        char *c1 = strchr(line, ',');
        if (!c1) continue;
        *c1 = 0;
        strcpy(name, line);
        trim(name);
        char *c2 = strchr(c1+1, ',');
        if (!c2) continue;
        lon = atof(c1+1);
        lat = atof(c2+1);
        strncpy(cities[cnt].name, name, sizeof(cities[cnt].name)-1);
        cities[cnt].lon = lon;
        cities[cnt].lat = lat;
        cnt++;
    }
    fclose(f);
    *n = cnt;
    return cnt > 0;
}

/* Load space-separated places file: Name LAT LON  (example uploaded file) */
int load_places_space(const char *fn, City *cities, int *n) {
    FILE *f = fopen(fn, "r");
    if (!f) return 0;
    char line[MAX_LINE];
    int cnt = 0;
    while (fgets(line, sizeof(line), f) && cnt < MAX_CITIES) {
        line[strcspn(line, "\n")] = 0;
        char *comment = strchr(line, '#');
        if (comment) *comment = 0;
        trim(line);
        if (strlen(line) == 0) continue;
        char name[128];
        double lat=0, lon=0;
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        char *q = p;
        while (*q && !isspace((unsigned char)*q)) q++;
        int name_len = q - p;
        if (name_len <= 0) continue;
        if (name_len >= (int)sizeof(name)) name_len = sizeof(name)-1;
        strncpy(name, p, name_len);
        name[name_len] = 0;
        p = q;
        while (*p && isspace((unsigned char)*p)) p++;
        char tok1[128] = {0}, tok2[128] = {0};
        int scanned = sscanf(p, "%127s %127s", tok1, tok2);
        if (scanned == 2) {
            lat = atof(tok1);
            lon = atof(tok2);
            strncpy(cities[cnt].name, name, sizeof(cities[cnt].name)-1);
            cities[cnt].lat = lat;
            cities[cnt].lon = lon;
            cnt++;
        } else {
            continue;
        }
    }
    fclose(f);
    *n = cnt;
    return cnt > 0;
}

/* Convenience: try places file first (space), then fallback to comma cities.txt */
int load_cities_auto(const char *places_fn, const char *cities_fn, City *cities, int *n) {
    if (places_fn && strlen(places_fn) > 0) {
        if (load_places_space(places_fn, cities, n)) {
            printf("✓ Loaded %d places from %s (space-separated format)\n", *n, places_fn);
            return 1;
        } else {
            printf("⚠ Could not load places from %s — falling back to %s\n", places_fn, cities_fn);
        }
    }
    if (load_cities_comma(cities_fn, cities, n)) {
        printf("✓ Loaded %d cities from %s (comma format)\n", *n, cities_fn);
        return 1;
    }
    return 0;
}

/* -------------------- TomTom sampling (uses embedded key) -------------------- */
double sample_tomtom_factor(double lat, double lon) {
#ifdef USE_TOMTOM
    char cmd[1024];
    char buf[8192];
    snprintf(cmd, sizeof(cmd),
      "curl -s \"https://api.tomtom.com/traffic/services/4/flowSegmentData/absolute/10/json?point=%f,%f&key=%s\"",
      lat, lon, TOMTOM_API_KEY);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 1.0;
    size_t r = fread(buf,1,sizeof(buf)-1,fp);
    buf[r] = 0;
    pclose(fp);
    double cur=-1, freef=-1;
    char *p = strstr(buf, "\"currentSpeed\"");
    if (p) sscanf(p, "\"currentSpeed\"%*[^:]:%lf", &cur);
    p = strstr(buf, "\"freeFlowSpeed\"");
    if (p) sscanf(p, "\"freeFlowSpeed\"%*[^:]:%lf", &freef);
    if (cur > 0 && freef > 0) {
        double fac = freef / cur;
        if (fac < 1.0) fac = 1.0;
        if (fac > 4.0) fac = 4.0;
        return fac;
    }
    return 1.0;
#else
    (void)lat; (void)lon;
    return 1.0;
#endif
}

/* -------------------- Graph builder -------------------- */

Edge *build_complete_graph(City *cities, int n) {
    Edge *edges = calloc(n * n, sizeof(Edge));
    if (!edges) { perror("calloc"); exit(1); }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            Edge *e = &edges[i*n + j];
            e->v = (i==j)? -1 : j;
            if (i != j) {
                double d = haversine_km(cities[i].lat, cities[i].lon, cities[j].lat, cities[j].lon);
                e->distance_km = d;
                e->traffic_factor = 1.0;
                e->co2_cost = 0.0;
            } else {
                e->distance_km = 0.0;
                e->traffic_factor = 1.0;
                e->co2_cost = 0.0;
            }
        }
    }
    return edges;
}

/* -------------------- Traffic cache helpers -------------------- */

int is_cache_fresh(const char *fn, int ttl_minutes) {
    FILE *f = fopen(fn, "r");
    if (!f) return 0;
    long long ts = 0;
    if (fscanf(f, "%lld", &ts) != 1) { fclose(f); return 0; }
    fclose(f);
    time_t now = time(NULL);
    long long age = (long long)now - ts;
    if (age < 0) return 0;
    if (age <= (long long)ttl_minutes * 60LL) return 1;
    return 0;
}

int load_traffic_cache(Graph *g) {
    FILE *f = fopen(TRAFFIC_CACHE_FILE, "r");
    if (!f) return 0;
    long long ts = 0;
    if (fscanf(f, "%lld\n", &ts) != 1) { fclose(f); return 0; }
    int n = g->n;
    for (int i = 0; i < n*n; ++i) g->edges[i].traffic_factor = 1.0;
    int u,v; double fac;
    while (fscanf(f, "%d %d %lf\n", &u, &v, &fac) == 3) {
        if (u >=0 && u < n && v >=0 && v < n) {
            g->edges[u*n + v].traffic_factor = fac;
            g->edges[v*n + u].traffic_factor = fac;
        }
    }
    fclose(f);
    return 1;
}

int save_traffic_cache(Graph *g) {
    FILE *f = fopen(TRAFFIC_CACHE_FILE, "w");
    if (!f) return 0;
    time_t now = time(NULL);
    fprintf(f, "%lld\n", (long long)now);
    int n = g->n;
    for (int i = 0; i < n; ++i) {
        for (int j = i+1; j < n; ++j) {
            double fac = g->edges[i*n + j].traffic_factor;
            fprintf(f, "%d %d %.6f\n", i, j, fac);
        }
    }
    fclose(f);
    return 1;
}

/* Build edge traffic factors with caching and optional forced refresh */
void build_edge_midpoint_traffic_factors_cached(Graph *g, int sample_every_n, int force_refresh, int ttl_minutes) {
    if (sample_every_n < 1) sample_every_n = SAMPLE_EVERY_N;
    if (!force_refresh && is_cache_fresh(TRAFFIC_CACHE_FILE, ttl_minutes)) {
        if (load_traffic_cache(g)) {
            printf("✓ Loaded traffic factors from cache '%s' (TTL %d min)\n", TRAFFIC_CACHE_FILE, ttl_minutes);
            return;
        }
    }
    /* Sample now (no valid cache or force_refresh) */
    int n = g->n;
    int sample_count = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = i+1; j < n; ++j) {
            int idx_ij = i*n + j;
            if ((sample_count % sample_every_n) == 0) {
#ifdef USE_TOMTOM
                double mlat = (g->cities[i].lat + g->cities[j].lat) / 2.0;
                double mlon = (g->cities[i].lon + g->cities[j].lon) / 2.0;
                double fac = sample_tomtom_factor(mlat, mlon);
                g->edges[idx_ij].traffic_factor = fac;
                g->edges[j*n + i].traffic_factor = fac;
                printf("Sampled traffic %d-%d : %.2fx\n", i, j, fac);
#else
                g->edges[idx_ij].traffic_factor = 1.0;
                g->edges[j*n + i].traffic_factor = 1.0;
#endif
            } else {
                g->edges[idx_ij].traffic_factor = 1.0;
                g->edges[j*n + i].traffic_factor = 1.0;
            }
            sample_count++;
        }
    }
    if (save_traffic_cache(g)) {
        printf("✓ Traffic cache saved to '%s'\n", TRAFFIC_CACHE_FILE);
    } else {
        printf("⚠️  Warning: failed to write traffic cache '%s'\n", TRAFFIC_CACHE_FILE);
    }
}

/* -------------------- Apply CO2 weights -------------------- */

void apply_co2_weights(Graph *g, double car_co2_g_per_km) {
    int n = g->n;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            Edge *e = &g->edges[i*n + j];
            if (e->v >= 0) {
                e->co2_cost = e->distance_km * e->traffic_factor * car_co2_g_per_km;
            } else e->co2_cost = 0.0;
        }
    }
}

/* -------------------- Dijkstra (min CO2) -------------------- */

int dijkstra(Graph *g, int src, int dst, int *out_path, int *out_len, double *out_cost) {
    int n = g->n;
    DijkNode *nodes = malloc(sizeof(DijkNode) * n);
    if (!nodes) { perror("malloc"); return 0; }
    for (int i = 0; i < n; ++i) { nodes[i].dist = INF; nodes[i].prev = -1; nodes[i].visited = 0; }
    nodes[src].dist = 0.0;

    for (;;) {
        int u = -1; double best = INF;
        for (int i = 0; i < n; ++i) if (!nodes[i].visited && nodes[i].dist < best) { best = nodes[i].dist; u = i; }
        if (u == -1) break;
        if (u == dst) break;
        nodes[u].visited = 1;
        for (int v = 0; v < n; ++v) {
            Edge *e = &g->edges[u*n + v];
            if (e->v >= 0 && !nodes[v].visited) {
                double alt = nodes[u].dist + e->co2_cost;
                if (alt < nodes[v].dist) { nodes[v].dist = alt; nodes[v].prev = u; }
            }
        }
    }

    if (nodes[dst].dist >= INF/2) { free(nodes); return 0; }

    int tmp[1024];
    int k = 0;
    int cur = dst;
    while (cur != -1 && k < 1024) {
        tmp[k++] = cur;
        cur = nodes[cur].prev;
    }
    int len = k;
    for (int i = 0; i < len; ++i) out_path[i] = tmp[len - 1 - i];
    *out_len = len;
    *out_cost = nodes[dst].dist;
    free(nodes);
    return 1;
}

/* -------------------- RDP Simplify helpers (new) -------------------- */

/* Simple 2D point for RDP */
typedef struct { double lat, lon; } Pt;

/* squared distance from point c to segment ab */
static double seg_point_dist2(Pt a, Pt b, Pt c){
    double vx = b.lon - a.lon;
    double vy = b.lat - a.lat;
    double wx = c.lon - a.lon;
    double wy = c.lat - a.lat;
    double vv = vx*vx + vy*vy;
    if (vv == 0.0) {
        double dx = c.lon - a.lon, dy = c.lat - a.lat;
        return dx*dx + dy*dy;
    }
    double t = (vx*wx + vy*wy) / vv;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    double px = a.lon + t*vx;
    double py = a.lat + t*vy;
    double dx = c.lon - px, dy = c.lat - py;
    return dx*dx + dy*dy;
}

/* RDP simplify: iterative stack-based implementation */
static int rdp_simplify(const Pt *pts, int n, double eps, Pt *out, int out_max) {
    if (n <= 0 || out_max <= 0) return 0;
    if (n == 1) { if (out_max>=1) { out[0]=pts[0]; return 1; } return 0; }

    int *stack = (int*)malloc(sizeof(int)*(n+5));
    int *keep = (int*)calloc(n, sizeof(int));
    if(!stack || !keep){ free(stack); free(keep); return 0; }

    int top = 0;
    stack[top++] = 0;
    stack[top++] = n-1;
    keep[0]=1; keep[n-1]=1;
    while (top>0) {
        int j = stack[--top];
        int i = stack[--top];
        double bestd2 = -1.0;
        int bestk = -1;
        for (int k = i+1; k < j; ++k) {
            double d2 = seg_point_dist2(pts[i], pts[j], pts[k]);
            if (d2 > bestd2) { bestd2 = d2; bestk = k; }
        }
        if (bestd2 > eps*eps) {
            keep[bestk] = 1;
            stack[top++] = i; stack[top++] = bestk;
            stack[top++] = bestk; stack[top++] = j;
        }
    }
    /* compose out */
    int c = 0;
    for (int i=0;i<n && c<out_max;i++) if (keep[i]) out[c++] = pts[i];
    free(stack); free(keep);
    return c;
}

/* -------------------- Interactive HTML output (simplified/speedy) -------------------- */
/* New signature includes car_co2 so JS displays exact numbers used in C */
/* Replacement write_html_map — builds interpolated points, simplifies in C, embeds simplified coords */
void write_html_map(const char *fn, Graph *g, int *path, int path_len, double total_co2,
                    double total_car_min, double total_bike_min, double total_walk_min, double car_co2) {
    FILE *f = fopen(fn, "w");
    if (!f) { perror("fopen html"); return; }

    /* tuning: samples per segment and simplify epsilon (degrees) */
    const int SAMPLES_PER_SEG = 8;      /* fewer samples -> faster */
    const double SIMPLIFY_EPS_DEG = 0.0004; /* ~0.04 deg ~ ~4.4 km at equator? (coarse); you can reduce to 0.00004 for ~4 m */
    /* Note: SIMPLIFY_EPS_DEG is in degrees — tune to taste. Smaller => more points, larger => fewer points. */

    /* Build raw interpolated points in C (Pt array) */
    int est_pts = (path_len > 1) ? ((path_len-1) * SAMPLES_PER_SEG + 1) : 1;
    Pt *raw = (Pt*)malloc(sizeof(Pt) * (est_pts + 8));
    if (!raw) { fclose(f); return; }
    int idx = 0;
    for (int i = 0; i < path_len - 1; ++i) {
        int u = path[i];
        int v = path[i+1];
        double lat1 = g->cities[u].lat;
        double lon1 = g->cities[u].lon;
        double lat2 = g->cities[v].lat;
        double lon2 = g->cities[v].lon;
        /* Interpolate SAMPLES_PER_SEG points between (including start, excluding end) */
        for (int s = 0; s < SAMPLES_PER_SEG; ++s) {
            double t = (double)s / (double)SAMPLES_PER_SEG;
            raw[idx].lat = lat1 * (1.0 - t) + lat2 * t;
            raw[idx].lon = lon1 * (1.0 - t) + lon2 * t;
            idx++;
        }
    }
    /* final endpoint explicitly */
    if (path_len > 0) {
        int last = path[path_len-1];
        raw[idx].lat = g->cities[last].lat;
        raw[idx].lon = g->cities[last].lon;
        idx++;
    }
    int raw_n = idx;

    /* Simplify raw -> simp */
    Pt *simp = (Pt*)malloc(sizeof(Pt) * raw_n);
    if (!simp) { free(raw); fclose(f); return; }
    int simp_n = rdp_simplify(raw, raw_n, SIMPLIFY_EPS_DEG, simp, raw_n);
    if (simp_n <= 0) { /* fallback: use raw */ simp_n = raw_n; for (int i=0;i<raw_n;i++) simp[i]=raw[i]; }

    /* Prepare node marker indices: pick a simplified index near each node */
    int *node_sample_idx = (int*)malloc(sizeof(int) * path_len);
    if (!node_sample_idx) { free(raw); free(simp); fclose(f); return; }
    for (int i=0; i<path_len; ++i) {
        double bestd = 1e18; int bi = 0;
        double nlat = g->cities[path[i]].lat, nlon = g->cities[path[i]].lon;
        for (int j=0;j<simp_n;j++){
            double dx = simp[j].lon - nlon, dy = simp[j].lat - nlat;
            double d2 = dx*dx + dy*dy;
            if (d2 < bestd) { bestd = d2; bi = j; }
        }
        node_sample_idx[i] = bi;
    }

    /* Write HTML */
    fprintf(f,
"<!doctype html>\n"
"<html>\n<head>\n<meta charset='utf-8'/>\n<meta name='viewport' content='width=device-width,initial-scale=1'/>\n"
"<title>CO2 Route (fast)</title>\n"
"<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'/>\n"
"<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>\n"
"<style>html,body,#map{height:100%%;margin:0} .panel{position:absolute;left:10px;top:10px;background:#fff;padding:10px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,.15);z-index:9999;font-family:sans-serif} .btn{display:inline-block;padding:6px 8px;background:#007bff;color:#fff;border-radius:6px;text-decoration:none;margin-right:6px}</style>\n"
"</head>\n<body>\n<div id='map'></div>\n"
"<div class='panel'><b>CO2-Optimized Route</b><br/>Total CO2: <span id='totalCo2'>%.2f</span> g<br/>Total time (car): <span id='totalCar'>%.1f</span> min<br/><div style='margin-top:8px'><a id='playBtn' class='btn'>Play</a><a id='pauseBtn' class='btn' style='background:#6c757d'>Pause</a><a id='downloadBtn' class='btn' style='background:#28a745'>Download GeoJSON</a></div></div>\n"
, total_co2, total_car_min);

    fprintf(f, "<script>\n");
    fprintf(f, "var map = L.map('map').setView([%f,%f], 12);\n", simp[0].lat, simp[0].lon);
    fprintf(f, "L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19, attribution:'&copy; OpenStreetMap'}).addTo(map);\n");

    /* embed simplified coords */
    fprintf(f, "var coordsAll = [\n");
    for (int i=0;i<simp_n;i++){
        fprintf(f, "  [%.7f, %.7f]%s\n", simp[i].lat, simp[i].lon, (i+1 < simp_n ? "," : ""));
    }
    fprintf(f, "];\n");

    /* node names */
    fprintf(f, "var nodeNames = [\n");
    for (int i=0;i<path_len;i++){
        char namebuf[256];
        strncpy(namebuf, g->cities[path[i]].name, sizeof(namebuf)-1); namebuf[sizeof(namebuf)-1]=0;
        for (char *p = namebuf; *p; ++p) if (*p == '"') *p = '\'';
        fprintf(f, "  \"%s\"%s\n", namebuf, (i+1<path_len ? "," : ""));
    }
    fprintf(f, "];\n");

    /* node indices */
    fprintf(f, "var nodeIdx = [");
    for (int i=0;i<path_len;i++){
        fprintf(f, "%d%s", node_sample_idx[i], (i+1<path_len? ",":""));
    }
    fprintf(f, "];\n");

    /* add markers and polyline */
    fprintf(f,
"for(var i=0;i<nodeIdx.length;i++){ var p = coordsAll[nodeIdx[i]]; if(p){ L.marker(p).addTo(map).bindPopup(nodeNames[i]); } }\n"
"var poly = L.polyline(coordsAll, {color:'#0066FF', weight:5, opacity:0.95, smoothFactor:1}).addTo(map);\n"
"try{ map.fitBounds(poly.getBounds().pad ? poly.getBounds().pad(0.12) : poly.getBounds(), {padding:[12,12]}); } catch(e){}\n"
);

    /* animation */
    fprintf(f,
"var animMarker = L.circleMarker(coordsAll[0], {radius:7, color:'#000'}).addTo(map);\n"
"var animT = 0, animPlaying=false;\n"
"function animate(){ if(!animPlaying) return; animT += 0.003; var idx = Math.floor(animT*(coordsAll.length-1)); if(idx >= coordsAll.length-1){ animT = 0; idx = 0; } var a = coordsAll[idx], b = coordsAll[Math.min(idx+1, coordsAll.length-1)]; var localT = (animT*(coordsAll.length-1)) - idx; var lat = a[0]*(1-localT) + b[0]*localT; var lon = a[1]*(1-localT) + b[1]*localT; animMarker.setLatLng([lat, lon]); requestAnimationFrame(animate); }\n"
"document.getElementById('playBtn').onclick = function(){ if(!animPlaying){ animPlaying=true; requestAnimationFrame(animate); } };\n"
"document.getElementById('pauseBtn').onclick = function(){ animPlaying=false; };\n"
);

    /* download geojson */
    fprintf(f,
"document.getElementById('downloadBtn').onclick = function(){ var feat = { type:'Feature', properties:{totalCo2: %.3f}, geometry:{ type:'LineString', coordinates: coordsAll.map(function(c){ return [c[1], c[0]]; }) } }; var geo = { type:'FeatureCollection', features:[feat] }; var data = 'data:application/json;charset=utf-8,' + encodeURIComponent(JSON.stringify(geo)); var a = document.createElement('a'); a.href = data; a.download = 'route.geojson'; document.body.appendChild(a); a.click(); document.body.removeChild(a); };\n"
, total_co2);

    /* embed mode times (for potential UI) */
    fprintf(f, "var totalCar = %.3f;\nvar totalBike = %.3f;\nvar totalWalk = %.3f;\n",
            total_car_min, total_bike_min, total_walk_min);

    fprintf(f,
"document.getElementById('modeSelect')?.addEventListener('change', function(){ var m = this.value; if(m=='car'){ document.getElementById('totalCar').textContent = totalCar.toFixed(1); } else if(m=='bike'){ document.getElementById('totalCar').textContent = totalBike.toFixed(1); } else { document.getElementById('totalCar').textContent = totalWalk.toFixed(1); } });\n"
"</script>\n</body>\n</html>\n");

    /* cleanup */
    free(raw); free(simp); free(node_sample_idx);
    fclose(f);
}

/* -------------------- Main -------------------- */

int shortp(){

    printf("\n=== MIN CO2 ROUTE (Dijkstra + Interactive Map) ===\n\n");

    /* ---- REMOVED ALL argc/argv parsing ---- */

    int force_refresh = 0;                 /* always use cache unless old */
    int ttl_minutes   = CACHE_TTL_MINUTES_DEFAULT;
    char places_file[512] = {0};           /* unused now */

    /* ---- Always load cities.txt ---- */
    if (access("cities.txt", F_OK) != 0) {
        printf("Error: 'cities.txt' not found.\n");
        printf("Example format:\nDehradun,78.0322,30.3165\nHaridwar,78.1642,29.9457\n\n");
        return 1;
    }

    /* Load cities */
    City cities[MAX_CITIES];
    int n = 0;
    if (!load_cities_comma("cities.txt", cities, &n)) {
        fprintf(stderr, "Failed to load cities.txt\n");
        return 1;
    }
    printf("Loaded %d locations\n", n);
    for (int i = 0; i < n; i++)
        printf("  %d: %s (lat %.6f lon %.6f)\n", i+1, cities[i].name, cities[i].lat, cities[i].lon);

    /* ---- User enters FROM and TO ---- */
    char route_input[256];
    char from_name[128], to_name[128];

    printf("\nEnter route (e.g. 'Dehradun to Delhi'):\n> ");
    if (!fgets(route_input, sizeof(route_input), stdin)) return 1;

    route_input[strcspn(route_input, "\n")] = 0;
    char *p = strstr(route_input, " to ");
    if (!p) { fprintf(stderr, "Invalid format. Use 'A to B'\n"); return 1; }
    *p = 0;

    strncpy(from_name, route_input, sizeof(from_name)-1);
    trim(from_name);

    strncpy(to_name, p+4, sizeof(to_name)-1);
    trim(to_name);

    int src = -1, dst = -1;
    for (int i=0;i<n;i++) {
        char ci[128];
        strncpy(ci, cities[i].name, sizeof(ci)-1); trim(ci);

        char cl[128], fl[128], tl[128];
        strcpy(cl, ci); for(char *q=cl;*q;q++) *q=tolower(*q);
        strcpy(fl, from_name); for(char *q=fl;*q;q++) *q=tolower(*q);
        strcpy(tl, to_name); for(char *q=tl;*q;q++) *q=tolower(*q);

        if (strcmp(cl, fl)==0) src = i;
        if (strcmp(cl, tl)==0) dst = i;
    }

    if (src < 0) { printf("City not found: %s\n", from_name); return 1; }
    if (dst < 0) { printf("City not found: %s\n", to_name); return 1; }

    printf("Found route: %s -> %s\n", cities[src].name, cities[dst].name);

    /* ---- Car model ---- */
    char car_model[128];
    printf("\nEnter car model (or press ENTER for Default):\n> ");
    if (!fgets(car_model, sizeof(car_model), stdin)) return 1;
    car_model[strcspn(car_model,"\n")]=0;
    if(strlen(car_model)==0) strcpy(car_model,"Default");

    double car_co2 = DEFAULT_CO2_GKM;
    FILE *cf = fopen("cars.txt","r");
    if (cf) {
        char line[MAX_LINE], model[128];
        double val;
        int found=0;
        while (fgets(line,sizeof(line),cf)) {
            if (sscanf(line,"%127[^,],%lf",model,&val)==2) {
                trim(model);
                char ml[128], cl[128];
                strcpy(ml,model); for(char*q=ml;*q;q++)*q=tolower(*q);
                strcpy(cl,car_model); for(char*q=cl;*q;q++)*q=tolower(*q);
                if(strcmp(ml,cl)==0){car_co2=val;found=1;break;}
            }
        }
        fclose(cf);

        if(!found)
            printf("Car model not found, using default %.1f g/km\n", car_co2);
    } else {
        printf("cars.txt not found; using default CO2\n");
    }

    printf("Using CO2 factor: %.2f g/km\n", car_co2);

    /* Build graph */
    Graph g;
    g.n = n;
    for(int i=0;i<n;i++) g.cities[i]=cities[i];
    g.edges = build_complete_graph(g.cities, g.n);

    printf("\nPreparing traffic factors (cache TTL = %d minutes)...\n", ttl_minutes);
    build_edge_midpoint_traffic_factors_cached(&g, SAMPLE_EVERY_N, force_refresh, ttl_minutes);

    apply_co2_weights(&g, car_co2);

    /* Run Dijkstra */
    int path[1024], path_len=0;
    double total_co2=0;

    if(!dijkstra(&g,src,dst,path,&path_len,&total_co2)){
        printf("No path found.\n");
        return 1;
    }

    /* Compute mode times */
    double total_car_min=0,total_bike_min=0,total_walk_min=0;

    printf("\nRoute steps:\n");
    for(int i=0;i<path_len-1;i++){
        int u=path[i], v=path[i+1];
        Edge*e=&g.edges[u*g.n+v];
        double d=e->distance_km;
        double factor=e->traffic_factor;

        double car_speed = CAR_FREEFLOW_KMPH/factor;
        if(car_speed<5) car_speed=5;

        double car_min=(d/car_speed)*60;
        double bike_min=(d/BIKE_KMPH)*60;
        double walk_min=(d/WALK_KMPH)*60;

        total_car_min+=car_min;
        total_bike_min+=bike_min;
        total_walk_min+=walk_min;

        printf("%s -> %s  %.2f km\n", g.cities[u].name,g.cities[v].name,d);
    }

    /* Write HTML */
    write_html_map(
        "route_co2_map.html", &g,
        path, path_len,
        total_co2,
        total_car_min, total_bike_min, total_walk_min,
        car_co2
    );

    open_in_browser("route_co2_map.html");
    free(g.edges);

    return 0;
}
