// Tiny synchronous pub/sub. The only channel core/ modules may use to
// signal features/ without importing them (no cycles, no off() needed).

const topics = new Map();

/** Subscribe fn(topic payload args). Runs inline during emit(). */
export function on(topic, fn) {
  if (!topics.has(topic)) topics.set(topic, []);
  topics.get(topic).push(fn);
}

/** Drop every subscriber of topic (a rebuilt shell re-registers its own). */
export function off(topic) {
  topics.delete(topic);
}

/** Notify every subscriber of topic, synchronously. */
export function emit(topic, ...args) {
  const fns = topics.get(topic);
  if (!fns) return;
  for (const fn of fns) fn(...args);
}
