// Double-fired-click guard shared by every submit/action handler. A
// second click/submit event dispatched before Svelte reactively applies a
// `disabled` binding can otherwise reach a handler twice before that
// handler's own synchronous "busy" state change has taken effect in the
// DOM.
//
// isBusy/markBusy are closures rather than a single owned flag because
// callers use different state shapes for "busy" - a plain boolean in
// most components, one value of a string state machine in
// Ota.svelte's upload()/WifiReset.svelte's resetWifi() (uploadState/
// wifiResetState also carry "success"/"error"/etc, not just busy/idle)
// - and every one of them already has other UI bindings (labels,
// `disabled` attributes) tied to that same variable, so this doesn't
// own or replace it. Only marks busy, never clears it - each caller's
// own outcome branches (success/error/idle) already decide what "not
// busy" means for its own state shape.
export function tripGuard(isBusy: () => boolean, markBusy: () => void): boolean {
  if (isBusy()) return true;
  markBusy();
  return false;
}
