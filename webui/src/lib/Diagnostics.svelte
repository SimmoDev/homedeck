<script lang="ts">
  // Diagnostics (see docs/architecture/web-ui.md#status) - three
  // unrelated concerns, each fully self-contained (own fetch, own
  // loading/error state, own peer-level section styling matching
  // Ota.svelte's own pattern), same "each section is fully
  // self-contained" reasoning Settings.svelte's own composition uses.
  import CrashDiagnostics from "./CrashDiagnostics.svelte";
  import DiagnosticsLogs from "./DiagnosticsLogs.svelte";
  import WifiReset from "./WifiReset.svelte";

  // onWifiReset fires once WifiReset.svelte's own reset action succeeds -
  // see its own onWifiReset comment for why that hands off to App.svelte
  // instead of rendering a "done" state locally.
  interface Props {
    onWifiReset: (apSsid: string) => void;
  }
  let { onWifiReset }: Props = $props();
</script>

<CrashDiagnostics />
<DiagnosticsLogs />
<WifiReset {onWifiReset} />
