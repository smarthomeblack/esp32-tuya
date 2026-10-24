// The entity editor: the card-gear dialog with the per-device entity
// checklist + remove (opened via the 'entity:edit' event).

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, $$, el, esc, icon } from '../core/utils.js';
import { on, emit } from '../core/events.js';
import { toast } from '../ui/toast.js';
import { confirmDialog, trapDialogFocus } from '../ui/dialogs.js';

/** The card gear dialog: entity checklist of an EXISTING device (EMPTY
 *  body = snapshot read), Save re-posts every row, danger = remove. */
async function openEntityEditor(devId) {
  const dev = state.devices.get(devId);
  if (!dev) return;
  const mask = el(`
    <div class="dlg-mask ent-mask">
      <div class="dlg ent-dlg" role="dialog" aria-modal="true" aria-labelledby="ent-dlg-title">
        <div class="ent-head">
          <h3 id="ent-dlg-title">${esc(dev.name)}</h3>
          <button class="ent-close" id="ent-close" aria-label="${t('dlg_cancel')}">${icon('close')}</button>
        </div>
        <div class="ent-body" id="ent-body">
          <div class="ent-loading"><div class="spin"></div></div>
        </div>
        <div class="ent-foot">
          <button class="btn" id="ent-save">${t('entity_save')}</button>
          <button class="btn-danger" id="ent-remove">${t('entity_remove')}</button>
        </div>
      </div>
    </div>`);
  document.body.appendChild(mask);
  requestAnimationFrame(() => mask.classList.add('show'));
  let release = () => {};
  const done = () => { release(); mask.remove(); };
  release = trapDialogFocus(mask, $('.ent-dlg', mask), done);
  $('#ent-close', mask).addEventListener('click', done);
  mask.addEventListener('click', e => { if (e.target === mask) done(); });

  let data = null;
  try {
    // EMPTY body: the handler answers the snapshot {"id","entities":[...]}.
    data = await api(`/api/devices/${encodeURIComponent(devId)}/entities`, { method: 'POST' });
  } catch (e) {
    done();   // the toast already carried the message
    return;
  }
  if (!document.body.contains(mask)) return;   // closed while fetching
  const ents = data && Array.isArray(data.entities) ? data.entities : [];
  const body = $('#ent-body', mask);
  body.innerHTML = `
    <div class="ent-hint">${t('entity_hint')}</div>
    ${ents.map(e => `
      <label class="ent-row">
        <input type="checkbox" data-ent="${esc(e.code)}"${e.enabled !== false ? ' checked' : ''}>
        <span class="cbx">${icon('check')}</span>
        <div class="ent-info">
          <span class="ent-name">${esc(e.name || e.code)}</span>
          <span class="ent-sub">${esc(e.code)}${e.type ? ' · ' + esc(e.type) : ''}</span>
        </div>
        ${e.writable ? `<span class="ent-w">${t('ent_writable')}</span>` : ''}
      </label>`).join('')}`;

  $('#ent-save', mask).addEventListener('click', async () => {
    // EVERY row, checked and unchecked — the full selection map.
    const inputs = $$('input[type="checkbox"][data-ent]', mask);
    if (!inputs.length) { done(); return; }
    const entities = {};
    for (const i of inputs) entities[i.getAttribute('data-ent')] = i.checked;
    const btn = $('#ent-save', mask);
    btn.disabled = true;
    try {
      await api(`/api/devices/${encodeURIComponent(devId)}/entities`, {
        method: 'POST', body: { entities },
      });
      toast(t('entity_saved'), 'ok');
      done();
      // Re-fetch /api/devices and force that card's rebuild even if the WS
      // event never arrives (entity toggle changes card structure).
      state.changeDevice = devId;
      emit('data:refresh', { silent: true });
    } catch (e) {
      btn.disabled = false;   // toast shown by api()
    }
  });

  $('#ent-remove', mask).addEventListener('click', async () => {
    const dev2 = state.devices.get(devId);
    const ok = await confirmDialog({
      title: t('entity_remove_confirm_title'),
      msg: t('entity_remove_confirm_msg', { name: dev2 ? dev2.name : devId }),
      ok: t('entity_remove'), danger: true,
    });
    if (!ok) return;
    try {
      await api(`/api/devices/${encodeURIComponent(devId)}/remove`, { method: 'POST', body: {} });
      toast(t('entity_removed'), 'ok');
      done();
      // device_removed already triggers scheduleDataRefresh; this
      // explicit refresh covers the no-WS case.
      emit('data:refresh', { silent: true });
    } catch (e) { /* toast shown */ }
  });
}

// The card gear opens the editor via the event bus.
on('entity:edit', devId => openEntityEditor(devId));
