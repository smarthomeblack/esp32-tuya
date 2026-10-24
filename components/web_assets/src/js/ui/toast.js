// Toast notifications (ok / err). Global single toast node.

import { $, el, icon } from '../core/utils.js';

let toastTimer = null;
/** kind: 'ok' | 'err'. */
export function toast(msg, kind) {
  let node = $('.toast');
  if (!node) {
    // The toast is the only live region — never #app (metric patches).
    node = el('<div class="toast" role="status" aria-live="polite" aria-atomic="true"><span class="t-ico"></span><span class="msg"></span></div>');
    document.body.appendChild(node);
  }
  const ico = kind === 'ok' ? 'check' : 'alert';
  $('.t-ico', node).innerHTML = icon(ico);
  $('.msg', node).textContent = msg;
  node.className = 'toast show ' + (kind || 'err');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => node.classList.remove('show'), 3500);
}
