// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Tuomas Airaksinen

/* Clay custom component "timerList": a variable-length list (max 16) of countdown
   timers, each a name + H/M/S duration, with a remove (x) button and an Add
   button. NO reordering (the watch sorts by most-recently-used). Component value
   is an array of { name: string, seconds: number }. Like config_widget_list.ts,
   this whole object is serialized via toSource() and re-eval'd in the config
   webview, so every function MUST be self-contained: no module-scope refs at
   runtime, no spread/destructuring, no TS downlevel helpers. Native DOM only. */

function timerListInitialize(this: any, _minified: any, _clayConfig: any): void {
  const self = this;
  const root: HTMLElement = self.$element[0];
  const MAX = 16;   // matches MAX_TIMERS in src/c/timer_calc.h

  function clamp(v: number, lo: number, hi: number): number {
    if (isNaN(v)) { return lo; }
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
  }

  // Build <option> 0..max, marking `sel` selected (and including sel as an extra
  // option if it somehow exceeds max, so a stored value is never silently lost).
  function selOptions(max: number, sel: number): string {
    let html = '';
    let found = false;
    for (let i = 0; i <= max; i++) {
      if (i === sel) { found = true; }
      html += '<option value="' + i + '"' + (i === sel ? ' selected' : '') + '>' + i + '</option>';
    }
    if (!found) { html += '<option value="' + sel + '" selected>' + sel + '</option>'; }
    return html;
  }

  function rowHtml(name: string, h: number, m: number, s: number): string {
    // textarea-safe: escape the name into the value attribute
    const safe = ('' + name).replace(/&/g, '&amp;').replace(/"/g, '&quot;')
      .replace(/</g, '&lt;').replace(/>/g, '&gt;');
    return '<div class="tl-row">' +
      '<input class="tl-name" type="text" placeholder="Name" value="' + safe + '">' +
      '<div class="tl-dur">' +
      '<select class="tl-h">' + selOptions(23, h) + '</select><span>h</span>' +
      '<select class="tl-m">' + selOptions(59, m) + '</select><span>m</span>' +
      '<select class="tl-s">' + selOptions(59, s) + '</select><span>s</span>' +
      '<button type="button" class="tl-del" title="Remove">&#10005;</button>' +
      '</div></div>';
  }

  function currentValue(): any[] {
    const out: any[] = [];
    const rows = root.querySelectorAll('.tl-row');
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i] as HTMLElement;
      const name = (r.querySelector('.tl-name') as HTMLInputElement).value;
      const h = parseInt((r.querySelector('.tl-h') as HTMLSelectElement).value, 10) || 0;
      const m = parseInt((r.querySelector('.tl-m') as HTMLSelectElement).value, 10) || 0;
      const s = parseInt((r.querySelector('.tl-s') as HTMLSelectElement).value, 10) || 0;
      out.push({ name: name, seconds: clamp(h, 0, 23) * 3600 + clamp(m, 0, 59) * 60 + clamp(s, 0, 59) });
    }
    return out;
  }

  function updateAdd(): void {
    const rows = root.querySelectorAll('.tl-row');
    const add = root.querySelector('.tl-add') as HTMLButtonElement;
    if (add) { add.style.display = (rows.length >= MAX) ? 'none' : ''; }
  }

  function rebuild(list: any[]): void {
    const listEl = root.querySelector('.tl-list') as HTMLElement;
    let html = '';
    for (let i = 0; i < list.length && i < MAX; i++) {
      const e = list[i] || {};
      let sec = parseInt(e.seconds, 10); if (isNaN(sec) || sec < 0) { sec = 0; }
      const h = Math.floor(sec / 3600);
      const m = Math.floor((sec - h * 3600) / 60);
      const s = sec - h * 3600 - m * 60;
      html += rowHtml(typeof e.name === 'string' ? e.name : '', h, m, s);
    }
    listEl.innerHTML = html;
    updateAdd();
  }

  self._tlCurrentValue = currentValue;
  self._tlRebuild = rebuild;

  function rowIndexOf(node: Node | null): number {
    const rows = root.querySelectorAll('.tl-row');
    for (let i = 0; i < rows.length; i++) { if (rows[i] === node) { return i; } }
    return -1;
  }

  root.addEventListener('click', function(ev: Event) {
    let target = ev.target as HTMLElement;
    if (!target) { return; }
    if (target.tagName !== 'BUTTON') {
      target = target.closest ? (target.closest('button') as HTMLElement) : null as any;
    }
    if (!target) { return; }
    if (target.classList.contains('tl-add')) {
      const v = currentValue();
      if (v.length < MAX) { v.push({ name: '', seconds: 300 }); rebuild(v); self.trigger('change'); }
      return;
    }
    if (target.classList.contains('tl-del')) {
      const rowEl = target.parentNode ? (target.parentNode as HTMLElement).parentNode as HTMLElement : null;
      const idx = rowIndexOf(rowEl);
      if (idx === -1) { return; }
      const v = currentValue();
      v.splice(idx, 1);
      rebuild(v);
      self.trigger('change');
    }
  });
}

const timerListComponent = {
  name: 'timerList',
  template:
    '<div class="tl-rootc">' +
    '<div class="tl-list"></div>' +
    '<button type="button" class="tl-add">+ Add timer</button>' +
    '</div>',
  // Clay base theme forces button{min-width:12rem;margin:0 auto}; row controls must
  // override that and dark-theme the native inputs to match Clay's dark page.
  style:
    '.tl-row{margin:0 0 12px 0}' +
    '.tl-name{display:block;width:100%;box-sizing:border-box;height:2.8rem;margin:0 0 4px 0;' +
      'background-color:#767676;color:#fff;border:none;border-radius:0.3rem;padding:0 0.5rem;color-scheme:dark}' +
    '.tl-dur{display:flex;align-items:center}' +
    '.tl-dur select{flex:1 1 auto;min-width:0;width:3rem;height:2.8rem;margin:0 2px 0 0;' +
      'background-color:#767676;color:#fff;border:none;border-radius:0.3rem;padding:0 0.3rem;color-scheme:dark}' +
    '.tl-dur span{flex:0 0 auto;margin:0 6px 0 0;color:#fff}' +
    '.tl-dur button{flex:0 0 auto;min-width:0;width:2.8rem;height:2.8rem;margin:0 0 0 6px;padding:0}' +
    '.tl-add{margin:8px 0 10px 0}',
  manipulator: {
    get: function(this: any): any[] {
      return this._tlCurrentValue ? this._tlCurrentValue() : [];
    },
    set: function(this: any, value: any) {
      let list: any[] = [];
      let v: any = value;
      if (v && typeof v === 'object' && !Array.isArray(v) && v.value !== undefined) { v = v.value; }
      if (Array.isArray(v)) {
        list = v;
      } else if (typeof v === 'string' && v !== '') {
        try { const p = JSON.parse(v); if (Array.isArray(p)) { list = p; } } catch (e) { list = []; }
      }
      if (this._tlRebuild) { this._tlRebuild(list); }
      return this;
    },
  },
  defaults: { label: '' },
  initialize: timerListInitialize,
};

export = timerListComponent;
