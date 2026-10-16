// =====================================================================
//  app.js — the browser side of the Expense Tracker
//
//  The page never touches expenses.csv directly. It only talks to the
//  C server through these HTTP requests ("the API"):
//
//    GET  /api/categories               -> list of categories
//    GET  /api/expenses?month=&category= -> list of expenses
//    POST /api/expenses                 -> add one (form fields)
//    POST /api/expenses/delete          -> delete one (id)
//    GET  /api/summary?month=           -> totals for the cards and bars
// =====================================================================

const rupees = new Intl.NumberFormat('en-IN', { style: 'currency', currency: 'INR' });
const $ = (sel) => document.querySelector(sel);

// Small wrapper around fetch(): returns parsed JSON or throws the server's error message.
async function api(path, options) {
  const res = await fetch(path, options);
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || `Request failed (${res.status})`);
  return data;
}

// URLSearchParams produces "a=1&b=2" and fetch() sends it as
// application/x-www-form-urlencoded — exactly what server.c parses.
const post = (path, fields) => api(path, { method: 'POST', body: new URLSearchParams(fields) });

function todayISO() {
  const d = new Date();
  const local = new Date(d.getTime() - d.getTimezoneOffset() * 60000);
  return local.toISOString().slice(0, 10);
}

function monthLabel(ym) {
  if (!ym) return 'All time';
  const [y, m] = ym.split('-').map(Number);
  return new Date(y, m - 1, 1).toLocaleDateString('en-IN', { month: 'long', year: 'numeric' });
}

// "16 Oct" for this year, "16 Oct 2025" for other years — keeps the table narrow.
function prettyDate(iso) {
  const [y, m, d] = iso.split('-').map(Number);
  const opts = { day: '2-digit', month: 'short' };
  if (y !== new Date().getFullYear()) opts.year = 'numeric';
  return new Date(y, m - 1, d).toLocaleDateString('en-IN', opts);
}

// ---------- State: what the filters currently say ----------
const state = {
  month: todayISO().slice(0, 7),   // start on the current month
  category: '',
};

// ---------- Rendering ----------
function renderStats(summary) {
  $('#total-label').textContent = `Spent · ${monthLabel(state.month)}`;
  $('#stat-total').textContent = rupees.format(summary.total);
  $('#stat-count').textContent = summary.count;
  $('#stat-avg').textContent = summary.count ? rupees.format(summary.total / summary.count) : '—';
  const top = [...summary.byCategory].sort((a, b) => b.total - a.total)[0];
  $('#stat-top').textContent = top && top.total > 0 ? top.category : '—';
}

function renderBars(listEl, items, labelOf, emptyText) {
  listEl.replaceChildren();
  const max = Math.max(0, ...items.map((i) => i.total));
  if (items.length === 0) {
    const li = document.createElement('li');
    li.className = 'none';
    li.textContent = emptyText;
    listEl.append(li);
    return;
  }
  for (const item of items) {
    const li = document.createElement('li');
    if (item.total === 0) li.className = 'zero';
    const name = document.createElement('span');
    name.textContent = labelOf(item);
    const amt = document.createElement('span');
    amt.className = 'amt';
    amt.textContent = rupees.format(item.total);
    const track = document.createElement('div');
    track.className = 'track';
    const fill = document.createElement('div');
    fill.className = 'fill';
    fill.style.width = max > 0 ? `${(item.total / max) * 100}%` : '0';
    track.append(fill);
    li.append(name, amt, track);
    listEl.append(li);
  }
}

function renderRows(expenses) {
  const tbody = $('#rows');
  tbody.replaceChildren();
  $('#empty').hidden = expenses.length > 0;

  for (const e of expenses) {
    const tr = document.createElement('tr');

    const date = document.createElement('td');
    date.className = 'date';
    date.textContent = prettyDate(e.date);

    const cat = document.createElement('td');
    const tag = document.createElement('span');
    tag.className = 'tag';
    tag.textContent = e.category;
    cat.append(tag);

    const note = document.createElement('td');
    note.className = 'note';
    note.textContent = e.note;           // textContent = safe, never runs as HTML

    const amt = document.createElement('td');
    amt.className = 'num';
    amt.textContent = rupees.format(e.amount);

    const act = document.createElement('td');
    act.className = 'num';
    const del = document.createElement('button');
    del.className = 'del';
    del.type = 'button';
    del.textContent = 'Delete';
    del.setAttribute('aria-label', `Delete ${e.category} expense of ${rupees.format(e.amount)} on ${e.date}`);
    del.addEventListener('click', () => removeExpense(e));
    act.append(del);

    tr.append(date, cat, note, amt, act);
    tbody.append(tr);
  }
}

// ---------- Loading data from the server ----------
async function refresh() {
  const q = new URLSearchParams();
  if (state.month) q.set('month', state.month);
  const listQ = new URLSearchParams(q);
  if (state.category) listQ.set('category', state.category);

  try {
    const [summary, list] = await Promise.all([
      api(`/api/summary?${q}`),
      api(`/api/expenses?${listQ}`),
    ]);
    renderStats(summary);
    renderBars($('#cat-bars'), summary.byCategory, (i) => i.category, 'No categories');
    renderBars($('#month-bars'), summary.byMonth, (i) => monthLabel(i.month), 'Nothing recorded yet');
    renderRows(list.expenses);
  } catch (err) {
    showMsg(`Could not reach the server: ${err.message}`, 'err');
  }
}

function showMsg(text, kind) {
  const el = $('#form-msg');
  el.textContent = text;
  el.className = `msg ${kind || ''}`;
}

// ---------- Actions ----------
async function addExpense(ev) {
  ev.preventDefault();
  const form = ev.target;
  const btn = $('#add-btn');
  btn.disabled = true;
  try {
    const saved = await post('/api/expenses', {
      date: form.date.value,
      category: form.category.value,
      amount: form.amount.value.trim().replace(/,/g, ''),
      note: form.note.value,
    });
    showMsg(`Added ${rupees.format(saved.amount)} · ${saved.category}`, 'ok');
    form.amount.value = '';
    form.note.value = '';
    form.amount.focus();
    // Jump to the month of the new expense so the user sees it appear.
    if (state.month && saved.date.slice(0, 7) !== state.month) setMonth(saved.date.slice(0, 7));
    await refresh();
  } catch (err) {
    showMsg(err.message, 'err');
  } finally {
    btn.disabled = false;
  }
}

async function removeExpense(e) {
  if (!confirm(`Delete ${rupees.format(e.amount)} (${e.category}) on ${prettyDate(e.date)}?`)) return;
  try {
    await post('/api/expenses/delete', { id: e.id });
    await refresh();
  } catch (err) {
    showMsg(err.message, 'err');
  }
}

function setMonth(value) {
  state.month = value;
  $('#filter-month').value = value;
  $('#all-time').classList.toggle('active', !value);
}

// ---------- Start-up ----------
async function init() {
  const { categories } = await api('/api/categories');
  for (const c of categories) {
    $('#f-category').append(new Option(c, c));
    $('#filter-category').append(new Option(c, c));
  }
  $('#f-date').value = todayISO();
  setMonth(state.month);

  $('#add-form').addEventListener('submit', addExpense);
  $('#filter-month').addEventListener('change', (e) => { setMonth(e.target.value); refresh(); });
  $('#all-time').addEventListener('click', () => { setMonth(''); refresh(); });
  $('#filter-category').addEventListener('change', (e) => { state.category = e.target.value; refresh(); });

  await refresh();
}

init().catch((err) => showMsg(`Could not start: ${err.message}`, 'err'));
