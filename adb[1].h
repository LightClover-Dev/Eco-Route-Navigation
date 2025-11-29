/* =========================================================================
   Shortest Route in Small City (KNN graph + Dijkstra + Yen K=2)
   Modules:
     (1) Input UX
     (2) Graph Builder
     (3) Shortest Paths
     (4) UI Map (HTML + Leaflet)
     (5) Result Display  <-- IMPROVED: adds times for car/bike/walk
   ========================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =============================== CONFIG ================================= */
#define MAXV    1500
#define NAMELEN 64
#define MAXE    (MAXV*16)
#define INF     1e18
#define PLACES_FILE "places.txt"

/* Average speeds (km/h) used for ETA estimates */
#define CAR_KMH 40.0
#define BIKE_KMH 15.0
#define WALK_KMH 5.0

/* ============================ SHARED DATA =============================== */
static int V = 0;
static char names[MAXV][NAMELEN];
static double lat[MAXV], lon[MAXV];

/* adjacency list using arrays */
static int head[MAXV];
static int to[MAXE], nxt[MAXE];
static double w[MAXE];
static int E = 0;

/* Path container */
typedef struct {
    int len;
    int nodes[MAXV];
    double cost;
} Path;

/* ============================== UTILS =================================== */
static void die(const char *m){ fprintf(stderr,"%s\n",m); exit(1); }

/* Haversine (km) for indices */
static double haversine_km_idx(int i, int j){
    const double R=6371.0;
    double la1=lat[i]*M_PI/180.0, la2=lat[j]*M_PI/180.0;
    double dlat=(lat[j]-lat[i])*(M_PI/180.0);
    double dlon=(lon[j]-lon[i])*(M_PI/180.0);
    double a=sin(dlat/2)*sin(dlat/2)+cos(la1)*cos(la2)*sin(dlon/2)*sin(dlon/2);
    double c=2*atan2(sqrt(a),sqrt(1-a));
    return R*c;
}

/* lowercase copy */
static void tolower_copy(const char *src, char *dst, size_t n){
    size_t i=0; for(; src[i] && i+1<n; ++i) dst[i]=(char)tolower((unsigned char)src[i]);
    dst[i]=0;
}

/* case-insensitive substring check */
static int ci_contains(const char *hay, const char *needle){
    char a[NAMELEN], b[NAMELEN];
    tolower_copy(hay,a,sizeof(a)); tolower_copy(needle,b,sizeof(b));
    return strstr(a,b)!=NULL;
}

/* Levenshtein distance (small strings) */
static int min3(int a,int b,int c){ return (a<b?(a<b?a:b):(b<c?b:c)); }
static int levenshtein_ci(const char *s1, const char *s2){
    char a[NAMELEN], b[NAMELEN];
    tolower_copy(s1,a,sizeof(a)); tolower_copy(s2,b,sizeof(b));
    int n=(int)strlen(a), m=(int)strlen(b);
    int *d = (int*)malloc((n+1)*(m+1)*sizeof(int));
    if(!d) return 9999;
    for(int i=0;i<=n;i++) d[i*(m+1)+0]=i;
    for(int j=0;j<=m;j++) d[0*(m+1)+j]=j;
    for(int i=1;i<=n;i++){
        for(int j=1;j<=m;j++){
            int cost=(a[i-1]==b[j-1])?0:1;
            d[i*(m+1)+j]=min3(
                d[(i-1)*(m+1)+j]+1,
                d[i*(m+1)+(j-1)]+1,
                d[(i-1)*(m+1)+(j-1)]+cost
            );
        }
    }
    int ans=d[n*(m+1)+m];
    free(d);
    return ans;
}

/* ======================= (1) INPUT UX MODULE ============================ */
static int ask_place_interactive(const char *prompt){
    char q[256];
    printf("\n%s (type a name or part of it, case-insensitive): ", prompt);
    if (scanf("%255s", q)!=1) die("Input error.");

    for(int i=0;i<V;i++) if (strcasecmp(names[i], q)==0){ printf("✔ Selected: %s\n", names[i]); return i; }

    int cand_idx[64], cc=0;
    for(int i=0;i<V && cc<64;i++) if (ci_contains(names[i], q)) cand_idx[cc++]=i;

    if (cc>0){
        printf("Found %d matches. Choose one by number:\n", cc);
        for(int k=0;k<cc;k++) printf("  %2d) %s\n", k+1, names[cand_idx[k]]);
        printf(": ");
        int pick=0; if (scanf("%d",&pick)!=1 || pick<1 || pick>cc) die("Bad selection.");
        printf("✔ Selected: %s\n", names[cand_idx[pick-1]]);
        return cand_idx[pick-1];
    }

    struct { int idx, dist; } best[5];
    for(int b=0;b<5;b++){ best[b].idx=-1; best[b].dist=9999; }
    for(int i=0;i<V;i++){
        int d=levenshtein_ci(names[i], q);
        for(int b=0;b<5;b++){
            if (d<best[b].dist){
                for(int s=4;s>b;s--) best[s]=best[s-1];
                best[b].idx=i; best[b].dist=d; break;
            }
        }
    }
    printf("No direct matches. Did you mean:\n");
    for(int b=0;b<5 && best[b].idx!=-1;b++) printf("  %2d) %s\n", b+1, names[best[b].idx]);
    printf(": ");
    int pick=0; if (scanf("%d",&pick)!=1 || pick<1 || pick>5 || best[pick-1].idx==-1) die("Bad selection.");
    printf("✔ Selected: %s\n", names[best[pick-1].idx]);
    return best[pick-1].idx;
}

/* ======================= (2) GRAPH BUILDER MODULE ======================= */
static void reset_graph(){ for(int i=0;i<MAXV;i++) head[i]=-1; E=0; }

static void add_edge(int u,int v,double ww){
    if(u<0||u>=V||v<0||v>=V||u==v) return;
    if(E+2>=MAXE) die("Edge capacity exceeded (raise MAXE).");
    to[E]=v; w[E]=ww; nxt[E]=head[u]; head[u]=E++;
    to[E]=u; w[E]=ww; nxt[E]=head[v]; head[v]=E++;
}

static void load_places(){
    FILE *fp=fopen(PLACES_FILE,"r");
    if(!fp){ perror("open places.txt"); exit(1); }
    V=0;
    while(V<MAXV && fscanf(fp,"%63s %lf %lf", names[V], &lat[V], &lon[V])==3) V++;
    fclose(fp);
    if(V<2) die("Need at least 2 places in places.txt (format: Name lat lon)");
    reset_graph();
}

static void build_knn_fixed(int k){
    reset_graph();
    if (k<1) k=1;
    if (V-1<k) k=V-1;

    double *dists=(double*)malloc(sizeof(double)*V);
    int *idx=(int*)malloc(sizeof(int)*V);
    if(!dists||!idx) die("Memory error in KNN.");

    for(int u=0;u<V;u++){
        for(int v=0;v<V;v++){ dists[v]=(u==v)?INF:haversine_km_idx(u,v); idx[v]=v; }
        for(int i=0;i<k && i<V;i++){
            int mi=i;
            for(int j=i+1;j<V;j++) if(dists[j]<dists[mi]) mi=j;
            double td=dists[i]; dists[i]=dists[mi]; dists[mi]=td;
            int ti=idx[i]; idx[i]=idx[mi]; idx[mi]=ti;
        }
        int kk=(k<V)?k:V;
        for(int i=0;i<kk;i++){ int v=idx[i]; if(v!=u) add_edge(u,v,haversine_km_idx(u,v)); }
    }
    free(dists); free(idx);
    printf("\n[Graph Builder] Built %d-NN undirected graph with %d edges (V=%d)\n", k, E/2, V);
}

/* ===================== (3) SHORTEST PATHS MODULE ======================== */
static int parent[MAXV];
static double distv[MAXV];
static int used[MAXV];

static int minQ(){
    double best=INF; int bi=-1;
    for(int i=0;i<V;i++) if(!used[i] && distv[i]<best){ best=distv[i]; bi=i; }
    return bi;
}

static double ecodijkstra(int s,int t){
    for(int i=0;i<V;i++){ distv[i]=INF; used[i]=0; parent[i]=-1; }
    distv[s]=0.0;
    for(;;){
        int u=minQ(); if(u==-1) break;
        used[u]=1; if(u==t) break;
        for(int e=head[u]; e!=-1; e=nxt[e]){
            int v=to[e];
            double alt=distv[u]+w[e];
            if(alt<distv[v]){ distv[v]=alt; parent[v]=u; }
        }
    }
    return distv[t];
}

static int build_path(int t,int *out){
    if(distv[t]>=INF/2) return 0;
    int tmp[MAXV], k=0;
    for(int v=t; v!=-1; v=parent[v]) tmp[k++]=v;
    for(int i=0;i<k;i++) out[i]=tmp[k-1-i];
    return k;
}

static int equal_paths(const Path *a, const Path *b){
    if(a->len!=b->len) return 0;
    for(int i=0;i<a->len;i++) if(a->nodes[i]!=b->nodes[i]) return 0;
    return 1;
}

/* Skip one specific undirected edge during relaxation */
static int skip_u=-1, skip_v=-1;

static double dijkstra_skip_edge(int s,int t){
    for(int i=0;i<V;i++){ distv[i]=INF; used[i]=0; parent[i]=-1; }
    distv[s]=0.0;
    for(;;){
        int u=minQ(); if(u==-1) break;
        used[u]=1; if(u==t) break;
        for(int e=head[u]; e!=-1; e=nxt[e]){
            int v=to[e];
            if((u==skip_u && v==skip_v) || (u==skip_v && v==skip_u)) continue;
            double alt=distv[u]+w[e];
            if(alt<distv[v]){ distv[v]=alt; parent[v]=u; }
        }
    }
    return distv[t];
}

/* Yen's K-shortest with K up to 2 (best + one alt) */
static int yen_k2_paths(int s,int t,Path *out){
    double best=ecodijkstra(s,t);
    if(best>=INF/2) return 0;
    out[0].cost=best;
    out[0].len=build_path(t,out[0].nodes);
    int count=1;

    Path A[64]; int Ac=0;
    Path *prev=&out[0];
    for(int i=0;i<prev->len-1;i++){
        skip_u=prev->nodes[i];
        skip_v=prev->nodes[i+1];
        double c=dijkstra_skip_edge(prev->nodes[i], t);
        if(c>=INF/2) continue;

        int tmp[MAXV], kk=0;
        for(int v=t; v!=-1; v=parent[v]) tmp[kk++]=v;
        int spur_len=kk, spur_nodes[MAXV];
        for(int z=0; z<kk; z++) spur_nodes[z]=tmp[kk-1-z];

        Path cand; cand.len=0; cand.cost=0.0;
        for(int j=0;j<=i;j++){
            cand.nodes[cand.len++]=prev->nodes[j];
            if(j>0) cand.cost += haversine_km_idx(prev->nodes[j-1], prev->nodes[j]);
        }
        for(int j=1;j<spur_len;j++){
            cand.nodes[cand.len++]=spur_nodes[j];
            cand.cost += haversine_km_idx(spur_nodes[j-1], spur_nodes[j]);
        }

        int dup=0;
        for(int p=0;p<Ac;p++) if(equal_paths(&A[p], &cand)){ dup=1; break; }
        if(!dup && Ac<64) A[Ac++]=cand;
    }

    skip_u = skip_v = -1; /* reset skip edge */

    if(Ac==0) return count;
    int besti=0;
    for(int i=1;i<Ac;i++) if(A[i].cost < A[besti].cost) besti=i;
    out[count++]=A[besti];
    return count;
}

/* ========================== (4) UI MAP MODULE =========================== */

/* escape string into JS-safe double-quoted string (very small routine) */
static void js_escape(const char *s, char *out, size_t outn){
    size_t i=0;
    for(; *s && i+2<outn; ++s){
        if(*s=='\\' || *s=='\"') { out[i++]='\\'; out[i++]=*s; }
        else if(*s=='\n' || *s=='\r') { out[i++]=' '; }
        else out[i++]=*s;
    }
    out[i]=0;
}

/* compute minutes from distance (km) and speed (km/h) */
static double compute_time_min(double distance_km, double speed_kmh){
    if (speed_kmh <= 0.0) return 0.0;
    return (distance_km / speed_kmh) * 60.0;
}

/* ----- Modified write_html: now supports drawing alternative route too ----- */
static void write_html(const char *fn, Path *paths, int K /* K == number of found routes, up to 2 */){
    FILE *f=fopen(fn,"w"); if(!f){ perror("html"); return; }
    int cidx=(paths[0].len>0)?paths[0].nodes[0]:0;

    /* Compute times for best route (and alternative if present) */
    double best_km = paths[0].cost;
    double best_car_min = compute_time_min(best_km, CAR_KMH);
    double best_bike_min = compute_time_min(best_km, BIKE_KMH);
    double best_walk_min = compute_time_min(best_km, WALK_KMH);

    double alt_km=0.0, alt_car_min=0.0, alt_bike_min=0.0, alt_walk_min=0.0;
    int has_alt = (K>1 && paths[1].len>0);
    if (has_alt) {
        alt_km = paths[1].cost;
        alt_car_min = compute_time_min(alt_km, CAR_KMH);
        alt_bike_min = compute_time_min(alt_km, BIKE_KMH);
        alt_walk_min = compute_time_min(alt_km, WALK_KMH);
    }

    fprintf(f,
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>Router</title>"
"<link rel='stylesheet' href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'/>"
"<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
"<style>html,body,#map{height:100%%;margin:0}"
".panel{position:absolute;left:10px;top:10px;background:#fff;padding:12px;border-radius:10px;"
"box-shadow:0 2px 12px rgba(0,0,0,.18);font-family:system-ui,-apple-system,Segoe UI,Roboto,'Helvetica Neue',Arial;max-width:420px;z-index:9999;} "
".best{background:#eaf6ff;margin:6px 0;padding:8px;border-radius:8px;font-weight:600}"
".alt{background:#fff9e6;margin:6px 0;padding:8px;border-radius:8px;border:1px solid #f0e6c8}"
".small{font-size:13px;color:#444}.muted{color:#666;font-size:12px;margin-top:6px}"
".modes{display:flex;gap:8px;margin-top:8px}.mode{flex:1;padding:6px;border-radius:6px;background:#f5f7fb;text-align:center}"
".mode .num{font-weight:700;font-size:14px}.title{font-size:14px;margin-bottom:6px}"
"</style></head><body><div id='map'></div>\n");

    /* Panel with best route + times */
    fprintf(f, "<div class='panel'>");
    fprintf(f, "<div class='title'><b>Best route (road-snapped)</b></div>\n");
    fprintf(f, "<div class='best'>Distance: %.3f km<br/>\n", best_km);
    fprintf(f, "<div class='modes'>");
    fprintf(f, "<div class='mode' title='Car'><div>🚗</div><div class='num'>%.0f min</div><div class='small'>by car</div></div>", best_car_min);
    fprintf(f, "<div class='mode' title='Bike'><div>🚴</div><div class='num'>%.0f min</div><div class='small'>by bike</div></div>", best_bike_min);
    fprintf(f, "<div class='mode' title='Walk'><div>🚶</div><div class='num'>%.0f min</div><div class='small'>on foot</div></div>", best_walk_min);
    fprintf(f, "</div>\n");

    fprintf(f, "<div style='margin-top:8px'>");
    for(int j=0;j<paths[0].len;j++){
        fprintf(f, "%s%s", names[paths[0].nodes[j]], (j+1<paths[0].len?" ➜ ":""));
    }
    fprintf(f, "</div></div>\n");

    if (has_alt){
        fprintf(f, "<div class='alt'><b>Alternative</b><br/>Distance: %.3f km<br/>\n", alt_km);
        fprintf(f, "<div class='modes'>");
        fprintf(f, "<div class='mode'><div>🚗</div><div class='num'>%.0f min</div><div class='small'>by car</div></div>", alt_car_min);
        fprintf(f, "<div class='mode'><div>🚴</div><div class='num'>%.0f min</div><div class='small'>by bike</div></div>", alt_bike_min);
        fprintf(f, "<div class='mode'><div>🚶</div><div class='num'>%.0f min</div><div class='small'>on foot</div></div>", alt_walk_min);
        fprintf(f, "</div>\n");
        fprintf(f, "<div style='margin-top:8px'>");
        for(int j=0;j<paths[1].len;j++){
            fprintf(f, "%s%s", names[paths[1].nodes[j]], (j+1<paths[1].len?" ➜ ":""));
        }
        fprintf(f, "</div></div>\n");
    }

    fprintf(f, "<div class='muted'>Note: times are approximate, using average speeds (car: %.0f km/h, bike: %.0f km/h, walk: %.0f km/h).</div>", CAR_KMH, BIKE_KMH, WALK_KMH);
    fprintf(f, "</div>\n"); /* panel end */

    fprintf(f,
"<script>\n"
"var map=L.map('map').setView([%f,%f],15);\n"
"L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png',"
"{maxZoom:19,attribution:'&copy; OpenStreetMap'}).addTo(map);\n",
        lat[cidx], lon[cidx]
    );

    /* route0 nodes array */
    fprintf(f, "var route0_nodes=[\n");
    for(int j=0;j<paths[0].len;j++){
        int v=paths[0].nodes[j];
        fprintf(f,"  {lat:%0.6f, lon:%0.6f}%s\n", lat[v], lon[v], (j+1<paths[0].len?",":""));
    }
    fprintf(f, "];\n");

    /* route0 names array (escaped) */
    fprintf(f, "var route0_names=[");
    for(int j=0;j<paths[0].len;j++){
        int v=paths[0].nodes[j];
        char safe[256]; js_escape(names[v], safe, sizeof(safe));
        fprintf(f,"\"%s\"%s", safe, (j+1<paths[0].len?",":""));
    }
    fprintf(f, "];\n");

    /* If alternative exists, write its nodes & names */
    if (has_alt) {
        fprintf(f, "var route1_nodes=[\n");
        for(int j=0;j<paths[1].len;j++){
            int v=paths[1].nodes[j];
            fprintf(f,"  {lat:%0.6f, lon:%0.6f}%s\n", lat[v], lon[v], (j+1<paths[1].len?",":""));
        }
        fprintf(f, "];\n");
        fprintf(f, "var route1_names=[");
        for(int j=0;j<paths[1].len;j++){
            int v=paths[1].nodes[j];
            char safe[256]; js_escape(names[v], safe, sizeof(safe));
            fprintf(f,"\"%s\"%s", safe, (j+1<paths[1].len?",":""));
        }
        fprintf(f, "];\n");
    } else {
        fprintf(f, "var route1_nodes=null;\nvar route1_names=null;\n");
    }

    fprintf(f,
"var primaryColor='#0066FF';\n"
"var altColor='#FF7A00';\n"
"function drawStraight(nodes, color){var latlngs=nodes.map(n=>[n.lat,n.lon]);return L.polyline(latlngs,{weight:6,color:color,opacity:1.0}).addTo(map);} \n"
"async function osrmRoute(a,b){\n"
"  var url=`https://router.project-osrm.org/route/v1/driving/${a.lon},${a.lat};${b.lon},${b.lat}?overview=full&geometries=geojson`;\n"
"  const r=await fetch(url); if(!r.ok) throw new Error('OSRM error');\n"
"  const j=await r.json(); if(!j.routes||!j.routes[0]) throw new Error('No routes');\n"
"  return j.routes[0].geometry.coordinates.map(c=>[c[1],c[0]]);\n"
"}\n"
"async function drawSnapped(nodes, color){\n"
"  var all=[];\n"
"  for(let i=0;i<nodes.length-1;i++){\n"
"    try{\n"
"      const seg=await osrmRoute(nodes[i], nodes[i+1]);\n"
"      if(all.length && seg.length && (all[all.length-1][0]===seg[0][0] && all[all.length-1][1]===seg[0][1])) seg.shift();\n"
"      all=all.concat(seg);\n"
"    }catch(e){ all.push([nodes[i].lat,nodes[i].lon],[nodes[i+1].lat,nodes[i+1].lon]); }\n"
"  }\n"
"  return L.polyline(all,{weight:6,color:color,opacity:0.95}).addTo(map);\n"
"}\n"
"(async function(){\n"
"  var layers=[];\n" /* collect polylines for fitBounds */ 
"  var nodes0=route0_nodes;\n" 
"  for(let i=0;i<nodes0.length;i++){ L.marker([nodes0[i].lat,nodes0[i].lon]).addTo(map).bindPopup(route0_names[i]); }\n"
"  try{ var pl0=await drawSnapped(nodes0, primaryColor); layers.push(pl0); }\n"
"  catch(e){ var pl0=drawStraight(nodes0, primaryColor); layers.push(pl0); }\n"
);

    if (has_alt) {
        fprintf(f,
"  var nodes1=route1_nodes;\n"
"  for(let i=0;i<nodes1.length;i++){ L.circleMarker([nodes1[i].lat,nodes1[i].lon],{radius:4,fillOpacity:1}).addTo(map).bindPopup(route1_names[i]); }\n"
"  try{ var pl1=await drawSnapped(nodes1, altColor); pl1.setStyle({dashArray:'8,6'}); layers.push(pl1); }\n"
"  catch(e){ var pl1=drawStraight(nodes1, altColor); pl1.setStyle({dashArray:'8,6'}); layers.push(pl1); }\n"
);
    }

    fprintf(f,
"  var fg=L.featureGroup(layers);\n"
"  if(layers.length) map.fitBounds(fg.getBounds(), {padding:[20,20]});\n"
"})();\n"
"</script></body></html>");
    fclose(f);
}

static void try_open(const char *fn){
#if defined(_WIN32) || defined(_WIN64)
    char cmd[512]; snprintf(cmd,sizeof(cmd),"start \"\" \"%s\"", fn); system(cmd);
#elif defined(__APPLE__)
    char cmd[512]; snprintf(cmd,sizeof(cmd),"open \"%s\"", fn); system(cmd);
#else
    char cmd[512]; snprintf(cmd,sizeof(cmd),"xdg-open \"%s\" >/dev/null 2>&1 &", fn); system(cmd);
#endif
}

/* ======================== (5) RESULT DISPLAY MODULE ===================== */
/* Prints:
   - 📌 Optimized route (best path, i.e., routes[0])
   - 📌 Total distance covered (km)
   - 📌 Approx times for car/bike/walk (minutes)
   Also optionally lists the alternative route if present.
*/
static void display_results(Path *routes, int found){
    printf("\n==================== Result Display ====================\n");
    /* BEST route summary */
    printf("📌 Optimized route: ");
    for (int j=0; j<routes[0].len; j++){
        if (j) printf(" -> ");
        printf("%s", names[routes[0].nodes[j]]);
    }
    printf("\n📌 Total distance covered: %.3f km\n", routes[0].cost);

    double best_km = routes[0].cost;
    double best_car_min = compute_time_min(best_km, CAR_KMH);
    double best_bike_min = compute_time_min(best_km, BIKE_KMH);
    double best_walk_min = compute_time_min(best_km, WALK_KMH);

    printf("Estimated times (approx):\n");
    printf("  🚗 Car  : %.0f min (avg %.0f km/h)\n", best_car_min, CAR_KMH);
    printf("  🚴 Bike : %.0f min (avg %.0f km/h)\n", best_bike_min, BIKE_KMH);
    printf("  🚶 Walk : %.0f min (avg %.0f km/h)\n", best_walk_min, WALK_KMH);

    /* Optional: show alternative route headline */
    if (found > 1){
        printf("\nAlternative route (for reference): ");
        for (int j=0; j<routes[1].len; j++){
            if (j) printf(" -> ");
            printf("%s", names[routes[1].nodes[j]]);
        }
        printf("\nDistance: %.3f km\n", routes[1].cost);

        double alt_km = routes[1].cost;
        double alt_car_min = compute_time_min(alt_km, CAR_KMH);
        double alt_bike_min = compute_time_min(alt_km, BIKE_KMH);
        double alt_walk_min = compute_time_min(alt_km, WALK_KMH);

        printf("Estimated times (alt):\n");
        printf("  🚗 Car  : %.0f min\n", alt_car_min);
        printf("  🚴 Bike : %.0f min\n", alt_bike_min);
        printf("  🚶 Walk : %.0f min\n", alt_walk_min);
    }
    printf("========================================================\n");
}

/* ================================ MAIN ================================== */
void ecopath(){
    /* (2) Graph builder: load + build */
    load_places();

    printf("Available places (%d):\n", V);
    for (int i=0; i<V; i++) printf("  %s\n", names[i]);

    int k = 8;
    if (V-1 < k) k = V-1;
    if (k < 2 && V >= 3) k = 2;
    build_knn_fixed(k);

    /* (1) Input UX for source & destination */
    int s = ask_place_interactive("Enter SOURCE");
    int t = ask_place_interactive("Enter DESTINATION");
    if (s == t) die("Source and destination must differ.");

    /* (3) Shortest paths */
    Path routes[2];
    int found = yen_k2_paths(s,t,routes);
    if (found == 0){
        printf("No route found (graph may be too sparse). Try adding places or increasing k.\n");
        return;
    }

    /* (5) Result Display: concise summary */
    display_results(routes, found);

    /* Optional detailed breakdown (segments) */
    for (int i=0; i<found; i++){
        printf("\nRoute %d detail (%.3f km):\n", i+1, routes[i].cost);
        for (int j=0; j<routes[i].len-1; j++){
            int a=routes[i].nodes[j], b=routes[i].nodes[j+1];
            printf("  %s -> %s : %.3f km\n", names[a], names[b], haversine_km_idx(a,b));
        }
    }

    /* (4) UI Map: BEST route to HTML, auto-open */
    const char *html="route_map.html";
    write_html(html, routes, found);
    printf("\nMap written to %s\n", html);
    try_open(html);
}
