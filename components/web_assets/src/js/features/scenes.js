// Scenes tab: the scene grid + trigger.

import { state } from '../core/state.js';
import { api } from '../core/api.js';
import { t } from '../core/i18n.js';
import { $, icon, esc } from '../core/utils.js';
import { toast } from '../ui/toast.js';

export function tabScenes() {
  const scenes = state.scenes
    .filter(s => state.homeFilter === 'all' || s.home_id === state.homeFilter);
  if (!scenes.length) {
    return `<div class="tab-panel"><div class="empty-state">
      ${icon('play')}<span>${t('scenes_empty')}</span>
    </div></div>`;
  }
  return `<div class="tab-panel"><div class="scenes-grid">
    ${scenes.map(s => `
      <button class="scene-tile${s.enabled ? '' : ' disabled'}" data-scene="${esc(s.id)}"${s.enabled ? '' : ' aria-disabled="true"'}>
        <span class="scene-ico">${icon('play')}</span>
        <span class="scene-name">${esc(s.name)}</span>
        ${s.enabled ? '' : `<span class="badge">${t('scene_disabled')}</span>`}
        <span class="ripple"></span>
      </button>`).join('')}
  </div></div>`;
}

/** One delegated click handler on the panel section (scenes.js). */
export function bindScenes(root) {
  const sec = root || $('[data-tab-sec="scenes"]');
  if (!sec) return;
  sec.addEventListener('click', async e => {
    const tile = e.target && e.target.closest ? e.target.closest('.scene-tile') : null;
    if (!tile) return;
    const id = tile.getAttribute('data-scene');
    // Disabled tiles stay focusable (AT announces aria-disabled) but the
    // click is a no-op.
    if (tile.classList.contains('disabled')) return;
    tile.classList.add('pulse');
    setTimeout(() => tile.classList.remove('pulse'), 650);
    try {
      await api(`/api/scenes/${encodeURIComponent(id)}/trigger`, {
        method: 'POST', body: { home_id: state.homeFilter === 'all' ? undefined : state.homeFilter },
      });
      const sc = state.scenes.find(s => s.id === id);
      toast(t('scene_running', { name: sc ? sc.name : '' }), 'ok');
    } catch (e) {
      toast(t('toast_scene_fail'), 'err');
    }
  });
}
