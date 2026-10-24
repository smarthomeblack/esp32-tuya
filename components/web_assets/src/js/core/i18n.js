// Dictionary loading, t() and setLang. Rendering is NOT done here —
// setLang emits 'lang:changed' and app.js persists + re-renders (this
// module must stay free of api/render imports to avoid cycles).

import { state } from './state.js';
import { emit } from './events.js';

export function t(key, vars) {
  const dict = state.dicts[state.lang] || {};
  let s = dict[key];
  if (s == null) s = (state.dicts.en && state.dicts.en[key]) || key;
  if (vars) {
    for (const k in vars) s = s.replace(new RegExp('\\{' + k + '\\}', 'g'), () => String(vars[k]));
  }
  return s;
}

export async function loadDict(lang) {
  if (state.dicts[lang]) return;
  const res = await fetch('/' + lang + '.json', { cache: 'no-cache' });
  if (!res.ok) throw new Error('dict ' + lang);
  state.dicts[lang] = await res.json();
}

/** Switch language, optionally persisting (settings toggle). Emits
 *  'lang:changed' { lang, persist } — app.js POSTs + applyTranslations. */
export async function setLang(lang, persist) {
  try {
    await loadDict(lang);
  } catch (e) {
    console.error('dict load failed', lang);
    return;
  }
  state.lang = lang;
  document.documentElement.lang = lang;
  if (persist) {
    try { localStorage.setItem('hub_lang', lang); } catch (e) { /* private mode */ }
  }
  emit('lang:changed', { lang, persist });
}
