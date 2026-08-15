<script lang="ts">
  // Settings/backup (see docs/architecture/web-ui.md#scope and
  // docs/decisions/ADR-0023-settings-backup-api.md) - each section below
  // is fully self-contained (own fetch, own loading/error state),
  // matching Diagnostics.svelte/Ota.svelte's existing pattern rather
  // than sharing one parent-fetched state.
  import BackupSettings from "./BackupSettings.svelte";
  import DeviceNameSettings from "./DeviceNameSettings.svelte";
  import HarmonySettings from "./HarmonySettings.svelte";
  import WeatherSettings from "./WeatherSettings.svelte";
</script>

<div class="settings">
  <h2>Settings</h2>
  <DeviceNameSettings />
  <WeatherSettings />
  <HarmonySettings />
  <BackupSettings />
</div>

<style>
  .settings {
    text-align: left;
    margin-top: 1.5rem;
    padding-top: 1.5rem;
    border-top: 1px solid #e5e7eb;
  }

  h2 {
    margin: 0 0 0.75rem;
    font-size: 1.125rem;
  }
</style>
