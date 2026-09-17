/* shared frontend logic — pure vanilla JS */
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
/* canonical post URL: /post/<slug>, falling back to ?id= for legacy posts */
const postURL = p => p.slug ? '/post/' + encodeURIComponent(p.slug) : '/post?id=' + encodeURIComponent(p.id);
let state = { q: '', cat: 'All', page: 1, per: 3, total: 0, cats: ['All'], posts: [], loading: false };

/* visitor country via free GeoIP (cached 7 days). Pure JS, no backend dependency. */
async function getGeo() {
  try {
    const c = JSON.parse(localStorage.getItem('gta6_geo') || 'null');
    if (c && c.cc && Date.now() - c.ts < 7 * 24 * 3600 * 1000) return c;
  } catch (e) {}
  const lookup = (async () => {
    try {
      const r = await fetch('https://ip-api.com/json/?fields=status,country,countryCode');
      const j = await r.json();
      if (j.status === 'success' && j.countryCode) return { cc: j.countryCode, country: j.country };
    } catch (e) {}
    try {
      const r = await fetch('https://ipapi.co/json/');
      const j = await r.json();
      if (j.country_code) return { cc: j.country_code, country: j.country_name };
    } catch (e) {}
    return { cc: 'XX', country: 'Unknown' };
  })();
  const g = await Promise.race([lookup, new Promise(res => setTimeout(() => res({ cc: 'XX', country: 'Unknown' }), 2500))]);
  try { localStorage.setItem('gta6_geo', JSON.stringify({ ...g, ts: Date.now() })); } catch (e) {}
  return g;
}

/* fire-and-forget page-view beacon for non-post pages (home/about) */
async function trackPage(page) {
  try {
    const g = await getGeo();
    fetch('/api/track', { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ page, cc: g.cc, country: g.country }), keepalive: true });
  } catch (e) {}
}

function goSearch(e) { e.preventDefault(); const v = document.getElementById('navSearch').value; document.getElementById('search').value = v; state.q = v; loadPosts(false); document.getElementById('latest').scrollIntoView({behavior:'smooth'}); return false; }
function cardHTML(p, featured, i) {
  const d = Math.min((i || 0) * 70, 560);
  const imgFallback = p.video
    ? `this.onerror=null;this.src='https://i.ytimg.com/vi/${p.video}/hqdefault.jpg'`
    : `this.onerror=null;this.src='https://picsum.photos/seed/gta6${p.id}/800/400'`;
  return `<div class="${featured ? 'col-md-6' : 'col-md-6 col-lg-4'}">
    <a href="${postURL(p)}" class="text-decoration-none"><div class="post-card enter" style="animation-delay:${d}ms">
      <div class="thumb-wrap"><img src="${esc(p.image)}" alt="" loading="lazy" onerror="${imgFallback}">
      ${p.video ? '<div class="play-badge"><i>▶</i></div>' : ''}</div>
      <div class="p-3">
        <span class="cat-badge">${esc(p.category)}</span>
        <span class="meta ms-2">${p.video ? `👁 ${p.views||0} • ` : ''}💬 ${p.comments||0}</span>
        <h5 class="mt-2 text-white">${p.video ? '▶ ' : ''}${p.featured ? '⭐ ' : ''}${esc(p.title)}</h5>
        <p class="excerpt small">${esc(p.excerpt)}</p>
        <div class="meta">📅 ${esc(p.date)} • ✍️ ${esc(p.author)}</div>
      </div>
    </div></a></div>`;
}
async function loadCats() {
  const r = await fetch('/api/posts?per=50'); const j = await r.json();
  const set = new Set(['All']); j.posts.forEach(p => p.category && set.add(p.category));
  state.cats = [...set];
  document.getElementById('cats').innerHTML = state.cats.map(c =>
    `<button class="cat-pill ${c===state.cat?'active':''}" onclick="setCat('${esc(c)}')">${esc(c)}</button>`).join('');
}
function setCat(c) { state.cat = c; loadCats(); loadPosts(false); }
/* shimmer placeholders while the feed loads */
function skelHTML() {
  let s = '';
  for (let i = 0; i < 3; i++) s += '<div class="col-md-6 col-lg-4"><div class="skel"><div class="ph" style="height:200px"></div><div class="p-3"><div class="ph" style="height:13px;width:35%;border-radius:4px"></div><div class="ph mt-2" style="height:19px;border-radius:4px"></div><div class="ph mt-2" style="height:13px;width:75%;border-radius:4px"></div></div></div></div>';
  return s;
}
/* gradual feed: page 1 first, more appended on scroll / button */
async function loadPosts(append) {
  if (state.loading) return;
  state.loading = true;
  if (!append) { state.page = 1; state.posts = []; document.getElementById('grid').innerHTML = skelHTML(); }
  const qs = new URLSearchParams({ search: state.q, category: state.cat, page: state.page, per: state.per });
  try {
    const r = await fetch('/api/posts?' + qs); const j = await r.json();
    state.total = j.total;
    state.posts = append ? state.posts.concat(j.posts) : j.posts;
    /* strict newest-first order (creation order = published order).
       No pinned featured row — featured posts just get a ⭐ in the list. */
    document.getElementById('featured').innerHTML = '';
    document.getElementById('grid').innerHTML = state.posts.map((p, i) => cardHTML(p, false, i % state.per)).join('');
    document.getElementById('empty').classList.toggle('d-none', state.posts.length > 0);
    document.getElementById('pageInfo').textContent = state.total ? `Showing ${state.posts.length} of ${state.total}` : '';
    document.getElementById('moreBtn').classList.toggle('d-none', state.posts.length >= state.total);
  } catch (e) {}
  state.loading = false;
}
function loadMore() {
  if (state.loading || state.posts.length >= state.total) return;
  state.page++;
  loadPosts(true);
}
async function loadStats() {
  /* homepage no longer shows counters (blog look); admin dashboard has them */
  if (!document.getElementById('statPosts')) return;
  try {
    const r = await fetch('/api/posts?per=50'); const j = await r.json();
    document.getElementById('statPosts').textContent = j.total;
    let v = 0, c = 0; j.posts.forEach(p => { v += p.views||0; c += p.comments||0; });
    document.getElementById('statViews').textContent = v;
    document.getElementById('statComments').textContent = c;
  } catch (e) {}
}
async function subscribe(e) {
  e.preventDefault();
  const em = document.getElementById('nlEmail').value.trim();
  const r = await fetch('/api/newsletter', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({ email: em }) });
  document.getElementById('nlMsg').textContent = r.ok ? '✅ Welcome to Leonida! Check your inbox.' : '❌ ' + (await r.json()).error;
  if (r.ok) document.getElementById('nlEmail').value = '';
  return false;
}
document.addEventListener('DOMContentLoaded', () => {
  /* scroll reveals (safe fallback if observer unsupported) */
  if (!('IntersectionObserver' in window)) {
    document.querySelectorAll('.reveal').forEach(el => el.classList.add('in'));
  } else {
    const io = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) { e.target.classList.add('in'); io.unobserve(e.target); } }), { threshold: .12 });
    document.querySelectorAll('.reveal').forEach(el => io.observe(el));
  }
  const s = document.getElementById('search');
  s.addEventListener('input', () => { state.q = s.value; loadPosts(false); });
  /* thin reading-progress bar */
  const bar = document.getElementById('progress');
  if (bar) addEventListener('scroll', () => {
    const h = document.documentElement;
    const max = h.scrollHeight - h.clientHeight;
    bar.style.width = (max > 0 ? (h.scrollTop / max) * 100 : 0) + '%';
  }, { passive: true });
  document.getElementById('moreBtn').onclick = loadMore;
  /* infinite scroll: load next chunk before reaching the bottom */
  const sentinel = document.getElementById('sentinel');
  if (sentinel && ('IntersectionObserver' in window)) {
    new IntersectionObserver(es => { if (es[0].isIntersecting) loadMore(); }, { rootMargin: '500px' }).observe(sentinel);
  }
  loadCats(); loadPosts(); loadStats(); trackPage('home');
  /* live feed: re-check for new posts every 30s + when tab regains focus */
  let lastSeen = '';
  async function checkNew() {
    try {
      const r = await fetch('/api/posts?per=1'); const j = await r.json();
      const sig = j.total + ':' + (j.posts.length ? j.posts[0].id : '');
      if (lastSeen && sig !== lastSeen) { loadCats(); loadPosts(false); }
      lastSeen = sig;
    } catch (e) {}
  }
  checkNew();
  setInterval(checkNew, 30000);
  document.addEventListener('visibilitychange', () => { if (!document.hidden) checkNew(); });
  window.addEventListener('focus', checkNew);
});
