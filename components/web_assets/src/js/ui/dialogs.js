// Promise-based confirm dialog + the shared modal a11y (focus move/trap,
// Escape, focus restore) reused by the entity editor.

import { $, $$, el, esc } from '../core/utils.js';
import { t } from '../core/i18n.js';

const FOCUSABLE = 'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])';
let dlgSeq = 0;

/** Masks append in open order — the last one is the topmost dialog. */
function isTopmost(mask) {
  const masks = $$('.dlg-mask');
  return masks[masks.length - 1] === mask;
}

/** Modal a11y: focus the first control (else the dialog box itself), trap
 *  Tab, close the topmost dialog on Escape, restore focus on close.
 *  Returns the release fn the caller runs when the dialog is removed. */
export function trapDialogFocus(mask, dlg, close) {
  const prev = document.activeElement;
  const focusables = () => $$(FOCUSABLE, dlg).filter(n => !n.disabled);
  const first = focusables()[0];
  if (first) first.focus();
  else { dlg.tabIndex = -1; dlg.focus(); }
  const onKey = e => {
    if (!isTopmost(mask)) return;
    if (e.key === 'Escape') { e.preventDefault(); close(); return; }
    if (e.key !== 'Tab') return;
    const list = focusables();
    if (!list.length) { e.preventDefault(); return; }
    const head = list[0], tail = list[list.length - 1];
    const act = document.activeElement;
    if (e.shiftKey && (act === head || !dlg.contains(act))) {
      e.preventDefault();
      tail.focus();
    } else if (!e.shiftKey && (act === tail || !dlg.contains(act))) {
      e.preventDefault();
      head.focus();
    }
  };
  document.addEventListener('keydown', onKey, true);
  return () => {
    document.removeEventListener('keydown', onKey, true);
    if (prev && prev !== document.body && prev.focus && document.contains(prev)) prev.focus();
  };
}

/** Resolve(true) on OK, false on cancel / mask click / Escape. */
export function confirmDialog(opts) {
  return new Promise(resolve => {
    const titleId = 'dlg-title-' + (++dlgSeq);
    const mask = el(`
      <div class="dlg-mask">
        <div class="dlg${opts.danger ? ' danger' : ''}" role="alertdialog" aria-modal="true" aria-labelledby="${titleId}">
          <h3 id="${titleId}">${esc(opts.title || '')}</h3>
          <p>${esc(opts.msg || '')}</p>
          <div class="dlg-btns">
            <button class="cancel">${t('dlg_cancel')}</button>
            <button class="ok${opts.danger ? ' danger' : ''}">${esc(opts.ok || t('dlg_confirm'))}</button>
          </div>
        </div>
      </div>`);
    let release = () => {};
    const done = val => { release(); mask.remove(); resolve(val); };
    document.body.appendChild(mask);
    release = trapDialogFocus(mask, $('.dlg', mask), () => done(false));
    $('.cancel', mask).addEventListener('click', () => done(false));
    $('.ok', mask).addEventListener('click', () => done(true));
    mask.addEventListener('click', e => { if (e.target === mask) done(false); });
    requestAnimationFrame(() => mask.classList.add('show'));
  });
}
