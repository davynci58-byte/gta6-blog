/* admin panel logic — pure vanilla JS */
const esc = s => String(s == null ? '' : s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const $ = id => document.getElementById(id);

async function me() {
  const r = await fetch('/api/me'); return (await r.json()).admin;
}
async function boot() {
  if (await me()) showDash(); else { $('loginView').classList.remove('d-none'); }
  $('lPass').addEventListener('keydown', e => { if (e.key === 'Enter') login(); });
}
async function login() {
  $('lMsg').textContent = 'Checking…';
  const r = await fetch('/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username: $('lUser').value, password: $('lPass').value }) });
  if (r.ok) showDash();
  else $('lMsg').textContent = '❌ ' + (await r.json()).error;
}
async function logout() { await fetch('/api/logout', { method: 'POST' }); location.reload(); }
function showDash() {
  $('loginView').classList.add('d-none'); $('dashView').classList.remove('d-none'); $('logoutBtn').classList.remove('d-none');
  loadStats(); loadPosts(); loadSubs(); loadAnalytics();
}
const flag = cc => cc === 'XX' ? '🌐' : [...cc].map(c => String.fromCodePoint(127397 + c.charCodeAt(0))).join('');
async function loadAnalytics() {
  const r = await fetch('/api/analytics'); if (!r.ok) return;
  const a = await r.json();
  $('anaUpdated').textContent = `Total visits: ${a.total_views} • Today: ${a.today_views} • Post views: ${a.post_views}`;
  const top = a.per_post.length ? a.per_post[0].views : 1;
  $('anaPosts').innerHTML = a.per_post.length ? a.per_post.map((p, i) => `
    <div><div class="d-flex justify-content-between gap-2">
      <span>${i === 0 ? '🥇' : i === 1 ? '🥈' : i === 2 ? '🥉' : `#${i + 1}`} <a href="/post?id=${p.id}" target="_blank">${esc(p.title).slice(0, 55)}</a></span>
      <b class="text-nowrap">${p.views} 👁 • ${p.comments} 💬</b>
    </div><div class="bar"><i style="width:${Math.max(2, Math.round(100 * p.views / top))}%"></i></div></div>`).join('')
    : '<span class="meta">No visits yet — share your blog!</span>';
  $('anaCountries').innerHTML = a.countries.length ? a.countries.map(c => `
    <div><div class="d-flex justify-content-between gap-2">
      <span>${flag(c.cc)} ${esc(c.name)}</span>
      <b class="text-nowrap">${c.views} (${c.pct}%)</b>
    </div><div class="bar teal"><i style="width:${Math.max(2, c.pct)}%"></i></div></div>`).join('')
    : '<span class="meta">No country data yet (ad-blocked GeoIP shows as 🌐 Unknown).</span>';
  const dmax = Math.max(1, ...a.daily.map(d => d.views));
  $('anaDaily').innerHTML = a.daily.length ? a.daily.map(d => `
    <div class="vbar" title="${d.date}: ${d.views} visits" style="height:${Math.max(4, Math.round(100 * d.views / dmax))}%">
      <span>${d.views}</span><em>${d.date.slice(5)}</em>
    </div>`).join('') : '<span class="meta">No data.</span>';
  const recent = (a.recent || []).slice(-20).reverse();
  $('anaRecent').innerHTML = recent.length ? recent.map(v => {
    const t = new Date(v.t * 1000).toISOString().slice(5, 16).replace('T', ' ');
    const page = /^\d+$/.test(v.p) ? `<a href="/post?id=${v.p}" target="_blank">post #${v.p}</a>` : esc(v.p);
    return `<tr><td class="text-nowrap">${t}</td><td>${page}</td><td>${flag(v.cc)} ${esc(v.n)}</td><td>${esc(v.ip)}</td></tr>`;
  }).join('') : '<tr><td colspan="4" class="meta">No visits logged yet.</td></tr>';
}
async function loadStats() {
  const r = await fetch('/api/stats'); if (!r.ok) return;
  const s = await r.json();
  $('stats').innerHTML = [
    ['📰 Posts', s.posts], ['👁 Total views', s.views],
    ['💬 Comments', s.comments], ['📬 Subscribers', s.subscribers],
  ].map(([k,v]) => `<div class="col-6 col-md-3"><div class="admin-card p-3 text-center"><div class="meta">${k}</div><div class="stat-num">${v}</div></div></div>`).join('');
}
async function loadPosts() {
  const j = await (await fetch('/api/posts?per=50')).json();
  $('postRows').innerHTML = j.posts.map(p => `<tr>
    <td>${p.id}</td><td><a href="/post?id=${p.id}" target="_blank">${esc(p.title).slice(0,60)}</a>${p.featured?' ⭐':''}</td>
    <td><span class="badge text-bg-secondary">${esc(p.category)}</span></td><td>${p.views||0} 👁 / ${p.comments||0} 💬</td>
    <td class="text-nowrap"><button class="btn btn-sm btn-ghost" onclick="openEdit('${p.id}')">Edit</button>
    <button class="btn btn-sm btn-danger" onclick="delPost('${p.id}')">Del</button></td></tr>`).join('');
  // comments preview across posts
  let html = '';
  for (const p of j.posts.slice(0, 6)) {
    const c = await (await fetch('/api/comments?post_id=' + p.id)).json();
    (c.comments || []).slice(0, 3).forEach((cm, i) => {
      html += `<div class="comment p-2"><b>${esc(cm.author)}</b> on <a href="/post?id=${p.id}">#${p.id}</a>: ${esc(cm.text).slice(0,90)}
        <button class="btn btn-sm btn-link text-danger p-0 ms-1" onclick="delComment('${p.id}',${i})">delete</button></div>`;
    });
  }
  $('allComments').innerHTML = html || 'No comments yet.';
}
async function loadSubs() {
  const r = await fetch('/api/subscribers'); if (!r.ok) return;
  const j = await r.json();
  $('subCount').textContent = j.emails.length;
  $('subs').innerHTML = j.emails.length ? j.emails.map(e => `<div>📧 ${esc(e)}</div>`).join('') : 'No subscribers yet.';
}
function openNew() {
  $('mTitle').textContent = 'New post'; $('fId').value = '';
  ['fTitle','fExcerpt','fImage','fTags','fContent','fVideo'].forEach(i => $(i).value = '');
  $('fAuthor').value = 'Kono'; $('fFeat').checked = false; $('fMsg').textContent = '';
}
async function openEdit(id) {
  const p = await (await fetch('/api/post?id=' + id + '&view=0')).json();
  $('mTitle').textContent = 'Edit #' + id; $('fId').value = id;
  $('fTitle').value = p.title; $('fCat').value = p.category; $('fExcerpt').value = p.excerpt;
  $('fImage').value = p.image; $('fVideo').value = p.video || ''; $('fTags').value = p.tags; $('fAuthor').value = p.author;
  $('fFeat').checked = !!p.featured; $('fContent').value = (p.content||'').replace(/\\n/g, '\n');
  new bootstrap.Modal(document.getElementById('postModal')).show();
}
async function savePost() {
  const id = $('fId').value;
  const payload = { title: $('fTitle').value.trim(), category: $('fCat').value, excerpt: $('fExcerpt').value.trim(),
    image: $('fImage').value.trim(), video: $('fVideo').value.trim(), tags: $('fTags').value.trim(), author: $('fAuthor').value.trim() || 'Kono',
    featured: $('fFeat').checked ? 1 : 0, content: $('fContent').value.trim() };
  if (!payload.title || !payload.content) { $('fMsg').textContent = 'Title + content required.'; return; }
  const url = id ? '/api/posts?id=' + id : '/api/posts';
  const r = await fetch(url, { method: id ? 'PUT' : 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload) });
  if (r.ok) { bootstrap.Modal.getInstance(document.getElementById('postModal')).hide(); loadPosts(); loadStats(); }
  else $('fMsg').textContent = '❌ ' + (await r.json()).error;
}
async function delPost(id) {
  if (!confirm('Delete post #' + id + '?')) return;
  await fetch('/api/posts?id=' + id, { method: 'DELETE' });
  loadPosts(); loadStats(); loadAnalytics();
}
async function delComment(pid, i) {
  if (!confirm('Delete comment?')) return;
  await fetch(`/api/comment?post_id=${pid}&index=${i}`, { method: 'DELETE' });
  loadPosts(); loadStats();
}
boot();
