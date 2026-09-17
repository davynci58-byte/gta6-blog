# 🌴 GTA6 Blog — pure C + FlashDB + Bootstrap

Fan-made Grand Theft Auto VI blog. Backend is **100% C** (POSIX sockets + pthreads, no framework).
Database is the real **[FlashDB](https://github.com/armink/FlashDB.git)** (`third_party/`, Linux file mode):
- **KVDB** → posts, comments, newsletter, sessions, counters
- **TSDB** → page-view / login / comment event log

Frontend is **pure HTML + CSS + JavaScript + Bootstrap 5** (CDN), GTA Vice-City neon theme.

## Features (all a blog needs)

| Page | URL |
|---|---|
| 🏠 Home w/ hero, featured, search, categories, pagination, videos, newsletter | `/` |
| 📰 Single post + video embed + views + share + related + comments | `/post?id=1` |
| ℹ️ About / timeline / FAQ | `/about` |
| 🔐 Admin panel (dashboard, analytics, CRUD, comments, subscribers, stats) | `/admin` |

Seed content is **official only** (no rumors): the Extended Look, Trailer 1,
Trailer 2 and an official-info post. Posts support an optional YouTube `video`
ID (validated `[A-Za-z0-9_-]{6,16}`) rendered as a nocookie embed with a
"Watch on YouTube" fallback link.

REST API: `GET /api/posts`, `GET /api/post?id=`, `GET/POST /api/comments`,
`POST /api/newsletter`, `POST /api/login`, `GET /api/stats` (admin),
`GET /api/analytics` (admin), `POST /api/track` (page-view beacon),
`POST/PUT/DELETE /api/posts` (admin) …

**Admin login:** user `kono` / pass `kono`

## 📊 Traffic analytics (`/admin`)

Every visit is tracked in FlashDB: per-post counters (`views_<id>`), total
(`site_views`), per-day buckets (`daily_YYYY-MM-DD`), per-country counters
(`ctr_<CC>` + name), a 60-entry recent-visits ring (`viewlog`, IPs masked as
`1.2.3.*`) and a TSDB event log (`view:<page>:<ip>:<CC>:<ts>`).

The dashboard shows:

- 🔢 total visits + today's visits
- 🥇 most-viewed post + full per-post ranking (views + comments, sorted)
- 🌍 visitors by country (flag, views, share %) — country is resolved in the
  browser via free GeoIP (`ip-api.com` → `ipapi.co` fallback, cached 7 days)
  and sent with the view; offline/ad-blocked visitors show as 🌐 Unknown
- 📅 visits for the last 14 days (bar chart)
- 🕵️ recent visits table (UTC time, page, country, masked IP)

## Quick start

```bash
make
./gta6-blog 8080
# open http://localhost:8080/  and  http://localhost:8080/admin
```

- `PORT=8080 ./gta6-blog` also works; `./gta6-blog [port] [public_dir]`
- Data lives in `./data/fdb_kvdb` + `./data/fdb_tsdb`. Delete `./data` to reset (re-seeds 6 GTA6 posts).
- FlashDB source: vendored from https://github.com/armink/FlashDB.git under `third_party/` (+ `include/fdb_cfg.h` enables KVDB+TSDB file-POSIX mode).

## Layout

```
gta6-blog/
├── src/server.c            # the whole backend (HTTP + blog logic + FlashDB glue)
├── include/fdb_cfg.h       # FlashDB config
├── third_party/inc|src/    # real FlashDB
├── public/                 # index.html post.html about.html admin.html css/ js/
├── Makefile
└── data/                   # created at runtime (FlashDB files)
```

## Notes

- `fdb_kv_get()` only supports ≤128B strings, so the server uses `fdb_kv_get_blob`/`fdb_kv_set_blob` for full post JSON (up to 24KB).
- Sessions: random hex token in cookie `session=…`, mirrored to KV `admin_session` so it survives restarts.
- TSDB `max_len=256` logs strings like `view:3:127.0.0.1:1726…`, `comment:2:…`, `post:create`, `login:ok`.
