<template>
  <section class="shell-dash" :aria-label="$t('shell_dashboard.title')">
    <div class="shell-section-head">
      <h2 class="mb-0">{{ $t('shell_dashboard.title') }}</h2>
      <span class="shell-helper ms-auto" v-if="updatedAt">{{ $t('shell_dashboard.updated', { time: formatClock(updatedAt) }) }}</span>
      <button type="button" class="btn btn-sm btn-link shell-icon-btn" :disabled="refreshing" @click="refresh(true)"
              :title="$t('shell_dashboard.refresh')" :aria-label="$t('shell_dashboard.refresh')">
        <i class="fas fa-rotate-right" :class="{ 'fa-spin': refreshing }"></i>
      </button>
    </div>

    <!-- Summary strip -->
    <dl class="shell-summary">
      <div class="shell-kv">
        <dt class="shell-label">{{ $t('shell_dashboard.host') }}</dt>
        <dd class="shell-num text-truncate" :title="hostName">{{ hostName || '-' }}</dd>
      </div>
      <div class="shell-kv">
        <dt class="shell-label">{{ $t('shell_dashboard.version') }}</dt>
        <dd class="shell-num">{{ version || '-' }}</dd>
      </div>
      <div class="shell-kv">
        <dt class="shell-label">{{ $t('shell_dashboard.platform') }}</dt>
        <dd class="text-capitalize">{{ platform || '-' }}</dd>
      </div>
      <div class="shell-kv">
        <dt class="shell-label">{{ $t('shell_dashboard.status') }}</dt>
        <dd v-if="appsFailed"><span class="shell-status shell-status-off"></span>{{ $t('shell_dashboard.unavailable') }}</dd>
        <!-- The host keeps the app running after clients disconnect, so "streaming" needs a client. -->
        <dd v-else-if="runningApp && connectedClients.length"><span class="shell-status shell-status-ok"></span>{{ $t('shell_dashboard.status_streaming') }}</dd>
        <dd v-else-if="runningApp"><span class="shell-status shell-status-off"></span>{{ $t('shell_dashboard.status_app_running') }}</dd>
        <dd v-else><span class="shell-status shell-status-off"></span>{{ $t('shell_dashboard.status_idle') }}</dd>
      </div>
      <div class="shell-kv">
        <dt class="shell-label">{{ $t('shell_dashboard.stream') }}</dt>
        <dd class="text-truncate" :title="runningApp">{{ runningApp || $t('shell_dashboard.stream_idle') }}</dd>
      </div>
      <div class="shell-kv">
        <dt class="shell-label">{{ $t('shell_dashboard.connected_clients') }}</dt>
        <dd class="shell-num" v-if="clients">{{ connectedClients.length }} / {{ clients.length }}</dd>
        <dd v-else>-</dd>
      </div>
    </dl>

    <!-- Live sessions (hidden on host builds without /api/shell/live) -->
    <div class="shell-panel" v-if="!liveFailed">
      <div class="shell-panel-head">
        <h3 class="mb-0">{{ $t('shell_dashboard.live') }}</h3>
        <span class="shell-helper" v-if="live && live.length">{{ $t('shell_dashboard.live_count', { n: live.length }) }}</span>
        <span class="shell-helper ms-auto" v-if="live && liveError">
          <span class="shell-status shell-status-warn"></span>{{ $t('shell_dashboard.live_stale') }}
        </span>
      </div>
      <p class="shell-helper shell-panel-desc">{{ $t('shell_dashboard.live_desc') }}</p>
      <div class="table-responsive shell-table-wrap">
        <table class="table table-hover shell-table">
          <thead>
            <tr>
              <th scope="col">{{ $t('shell_dashboard.col_device') }}</th>
              <th scope="col">{{ $t('shell_dashboard.col_app') }}</th>
              <th scope="col" class="text-end">{{ $t('shell_dashboard.col_duration') }}</th>
              <th scope="col" class="text-end">{{ $t('shell_dashboard.col_video') }}</th>
              <th scope="col" class="text-end">{{ $t('shell_dashboard.col_bitrate') }}</th>
              <th scope="col" class="text-end">{{ $t('shell_dashboard.col_rtt') }}</th>
              <th scope="col" class="text-end">{{ $t('shell_dashboard.col_recovery') }}</th>
              <th scope="col"><span class="visually-hidden">{{ $t('shell_dashboard.live_disconnect') }}</span></th>
            </tr>
          </thead>
          <tbody>
            <tr v-if="!live && liveError"><td colspan="8" class="shell-helper">{{ $t('shell_dashboard.unavailable') }}</td></tr>
            <tr v-else-if="!live"><td colspan="8" class="shell-helper">{{ $t('shell_dashboard.loading') }}</td></tr>
            <tr v-else-if="!live.length"><td colspan="8" class="shell-helper">{{ $t('shell_dashboard.no_live') }}</td></tr>
            <tr v-for="s in live" :key="liveKey(s)">
              <td class="shell-name-cell">
                <span class="shell-status" :class="s.state === 'running' ? 'shell-status-ok' : 'shell-status-off'"></span>
                {{ s.client || $t('pin.unpair_single_unknown') }}
              </td>
              <td>{{ s.app || '-' }}</td>
              <td class="shell-num text-end text-nowrap">{{ formatDuration(s.duration_s) }}</td>
              <td class="shell-num text-end text-nowrap" :title="videoDetail(s)">{{ formatVideo(s) }}</td>
              <td class="shell-num text-end text-nowrap" :title="liveBitrateDetail(s)">{{ formatBitrate(s.bitrate_kbps) }}</td>
              <td class="shell-num text-end text-nowrap">{{ s.rtt_ms ? `${s.rtt_ms} ms` : '-' }}</td>
              <td class="shell-num text-end text-nowrap" :title="recoveryDetail(s)">
                <span class="shell-status" :class="recoveryLevel(s)"></span>{{ recoveryPerMinute(s) }}
              </td>
              <td class="text-end">
                <button type="button" class="btn btn-sm btn-link shell-icon-btn" v-if="s.uuid && s.state === 'running'" :disabled="disconnecting === s.uuid"
                        @click="disconnectLive(s)" :title="$t('shell_dashboard.live_disconnect')" :aria-label="$t('shell_dashboard.live_disconnect')">
                  <i class="fas fa-link-slash"></i>
                </button>
                <span class="shell-helper ms-1" v-if="disconnectError === s.uuid">{{ $t('shell_dashboard.live_disconnect_failed') }}</span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <!-- Virtual display (hosts with the virtual display driver) -->
    <div class="shell-panel" v-if="vdisplay && vdisplay.driver">
      <div class="shell-panel-head">
        <h3 class="mb-0">{{ $t('shell_dashboard.vdisplay') }}</h3>
      </div>
      <div class="shell-vd-row">
        <div class="shell-vd-state">
          <div>
            <span class="shell-status" :class="vdisplay.active ? 'shell-status-ok' : 'shell-status-off'"></span>
            <template v-if="vdisplay.active">
              {{ $t('shell_dashboard.vdisplay_active') }}
              <span class="shell-num shell-helper ms-1" v-if="vdisplay.name">{{ vdisplay.name }}</span>
            </template>
            <template v-else-if="vdisplay.released">{{ $t('shell_dashboard.vdisplay_released') }}</template>
            <template v-else>{{ $t('shell_dashboard.vdisplay_unused') }}</template>
          </div>
          <div class="shell-helper mt-1">{{ vdisplayHelper }}</div>
          <div class="shell-helper mt-1" v-if="vdisplayError || vdisplay.release_failed">{{ $t('shell_dashboard.vdisplay_release_failed') }}</div>
        </div>
        <button type="button" class="btn btn-sm btn-secondary" v-if="vdisplay.active"
                :disabled="releasingVd || vdisplay.release_requested" @click="releaseVirtualDisplay">
          <i class="fas fa-display"></i>
          <span class="ms-1">{{ releasingVd || vdisplay.release_requested ? $t('shell_dashboard.vdisplay_releasing') : $t('shell_dashboard.vdisplay_release') }}</span>
        </button>
      </div>
    </div>

    <!-- Paired devices -->
    <div class="shell-panel">
      <div class="shell-panel-head">
        <h3 class="mb-0">{{ $t('shell_dashboard.devices') }}</h3>
        <span class="shell-helper" v-if="clients">{{ $t('shell_dashboard.devices_count', { n: clients.length }) }}</span>
        <a class="shell-panel-link ms-auto" href="./pin">{{ $t('shell_dashboard.manage_devices') }}</a>
      </div>
      <div class="table-responsive shell-table-wrap">
        <table class="table table-hover shell-table">
          <thead>
            <tr>
              <th scope="col">{{ $t('shell_dashboard.col_name') }}</th>
              <th scope="col">{{ $t('shell_dashboard.col_status') }}</th>
              <th scope="col">{{ $t('shell_dashboard.col_permissions') }}</th>
              <th scope="col">{{ $t('shell_dashboard.col_note') }}</th>
            </tr>
          </thead>
          <tbody>
            <tr v-if="clientsFailed"><td colspan="4" class="shell-helper">{{ $t('shell_dashboard.unavailable') }}</td></tr>
            <tr v-else-if="!clients"><td colspan="4" class="shell-helper">{{ $t('shell_dashboard.loading') }}</td></tr>
            <tr v-else-if="!clients.length"><td colspan="4" class="shell-helper">{{ $t('shell_dashboard.no_devices') }}</td></tr>
            <tr v-for="c in sortedClients" :key="c.uuid">
              <td class="shell-name-cell">{{ c.name || $t('pin.unpair_single_unknown') }}</td>
              <td class="text-nowrap">
                <span class="shell-status" :class="c.connected ? 'shell-status-ok' : 'shell-status-off'"></span>
                {{ c.connected ? $t('shell_dashboard.device_connected') : $t('shell_dashboard.device_offline') }}
              </td>
              <td class="text-nowrap">{{ permissionSummary(c.perm) }}</td>
              <td class="text-nowrap">
                <span v-if="c.display_mode">{{ $t('shell_dashboard.note_display_mode') }} <span class="shell-num">{{ c.display_mode }}</span></span>
                <span v-else class="shell-helper">-</span>
              </td>
            </tr>
          </tbody>
        </table>
      </div>
    </div>

    <!-- Recent sessions -->
    <div class="shell-panel">
      <div class="shell-panel-head">
        <h3 class="mb-0">{{ $t('shell_dashboard.sessions') }}</h3>
        <span class="shell-helper" v-if="sessions && sessions.length">{{ $t('shell_dashboard.sessions_count', { n: sessions.length }) }}</span>
      </div>
      <p class="shell-helper shell-panel-desc" v-if="!sessionsFailed">{{ $t('shell_dashboard.sessions_desc') }}</p>

      <p class="shell-helper shell-panel-desc" v-if="sessionsFailed">{{ $t('shell_dashboard.sessions_unavailable') }}</p>
      <template v-else>
        <div class="table-responsive shell-table-wrap">
          <table class="table table-hover shell-table">
            <thead>
              <tr>
                <th scope="col">{{ $t('shell_dashboard.col_started') }}</th>
                <th scope="col">{{ $t('shell_dashboard.col_device') }}</th>
                <th scope="col">{{ $t('shell_dashboard.col_app') }}</th>
                <th scope="col" class="text-end">{{ $t('shell_dashboard.col_duration') }}</th>
                <th scope="col" class="text-end">{{ $t('shell_dashboard.col_video') }}</th>
                <th scope="col" class="text-end">{{ $t('shell_dashboard.col_bitrate') }}</th>
                <th scope="col">{{ $t('shell_dashboard.col_end') }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="!sessions"><td colspan="7" class="shell-helper">{{ $t('shell_dashboard.loading') }}</td></tr>
              <tr v-else-if="!sessions.length"><td colspan="7" class="shell-helper">{{ $t('shell_dashboard.no_sessions') }}</td></tr>
              <tr v-for="(s, i) in visibleSessions" :key="`${s.started}-${s.client_uuid || s.client}-${i}`">
                <td class="text-nowrap">
                  <span class="shell-num">{{ formatStamp(s.started) || '-' }}</span>
                  <span class="shell-helper ms-2" v-if="relativeTime(s.started)">{{ relativeTime(s.started) }}</span>
                </td>
                <td class="shell-name-cell">{{ s.client || $t('pin.unpair_single_unknown') }}</td>
                <td>{{ s.app || '-' }}</td>
                <td class="shell-num text-end text-nowrap">{{ formatDuration(s.duration_s) }}</td>
                <td class="shell-num text-end text-nowrap" :title="videoDetail(s)">{{ formatVideo(s) }}</td>
                <td class="shell-num text-end text-nowrap" :title="pacingDetail(s)">{{ formatBitrate(s.bitrate_kbps) }}</td>
                <td class="text-nowrap" :title="endReasonDetail(s.end_reason)">
                  <template v-if="s.end_reason">
                    <span class="shell-status" :class="s.end_reason === 'timeout' ? 'shell-status-warn' : 'shell-status-off'"></span>
                    {{ endReason(s.end_reason) }}
                  </template>
                  <span v-else class="shell-helper">-</span>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <button type="button" class="btn btn-sm btn-link mt-2" v-if="sessions && sessions.length > sessionsCollapsedCount"
                @click="showAllSessions = !showAllSessions">
          {{ showAllSessions ? $t('shell_dashboard.show_less') : $t('shell_dashboard.show_more', { n: sessions.length }) }}
        </button>
      </template>
    </div>

    <!-- Install backups -->
    <div class="shell-panel">
      <div class="shell-panel-head">
        <h3 class="mb-0">{{ $t('shell_dashboard.backups') }}</h3>
        <span class="shell-helper" v-if="backups && backups.length">{{ $t('shell_dashboard.backups_count', { n: backups.length }) }}</span>
      </div>
      <p class="shell-helper shell-panel-desc" v-if="!backupsFailed">{{ $t('shell_dashboard.backups_desc') }}</p>

      <p class="shell-helper shell-panel-desc" v-if="backupsFailed">{{ $t('shell_dashboard.backups_unavailable') }}</p>
      <template v-else>
        <div class="table-responsive shell-table-wrap">
          <table class="table table-hover shell-table">
            <thead>
              <tr>
                <th scope="col">{{ $t('shell_dashboard.col_created') }}</th>
                <th scope="col" class="text-end">{{ $t('shell_dashboard.col_restores') }}</th>
                <th scope="col" class="text-end">{{ $t('shell_dashboard.col_replaced_by') }}</th>
                <th scope="col">{{ $t('shell_dashboard.col_status') }}</th>
                <th scope="col" class="text-end">{{ $t('shell_dashboard.col_actions') }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-if="!backups"><td colspan="5" class="shell-helper">{{ $t('shell_dashboard.loading') }}</td></tr>
              <tr v-else-if="!backups.length"><td colspan="5" class="shell-helper">{{ $t('shell_dashboard.no_backups') }}</td></tr>
              <template v-for="b in visibleBackups" :key="b.name">
                <tr>
                  <td class="text-nowrap">
                    <span class="shell-num">{{ formatStamp(b.created) || b.name }}</span>
                    <span class="shell-helper ms-2" v-if="relativeTime(b.created)">{{ relativeTime(b.created) }}</span>
                  </td>
                  <td class="shell-num text-end">{{ b.version || '-' }}</td>
                  <td class="shell-num text-end">{{ b.replaced_by || '-' }}</td>
                  <td class="text-nowrap" :title="b.restorable ? '' : $t('shell_dashboard.backup_not_restorable')">
                    <span class="shell-status" :class="b.restorable ? 'shell-status-ok' : 'shell-status-warn'"></span>
                    {{ b.restorable ? $t('shell_dashboard.backup_restorable') : $t('shell_dashboard.backup_not_restorable_short') }}
                  </td>
                  <td class="text-end text-nowrap">
                    <button type="button" class="btn btn-sm btn-secondary" v-if="b.restorable" @click="copyRollback(b)">
                      <i class="fas fa-fw" :class="copyState[b.name] === 'ok' ? 'fa-check' : 'fa-copy'"></i>
                      <span class="ms-1">{{ copyState[b.name] === 'ok' ? $t('shell_dashboard.copied') : $t('shell_dashboard.copy_rollback') }}</span>
                    </button>
                  </td>
                </tr>
                <tr v-if="copyState[b.name] === 'failed'" class="shell-command-row">
                  <td colspan="5">
                    <div class="shell-helper mb-1">{{ $t('shell_dashboard.copy_failed') }}</div>
                    <code class="shell-command" tabindex="0">{{ rollbackCommand(b) }}</code>
                  </td>
                </tr>
              </template>
            </tbody>
          </table>
        </div>
        <button type="button" class="btn btn-sm btn-link mt-2" v-if="backups && backups.length > collapsedCount"
                @click="showAllBackups = !showAllBackups">
          {{ showAllBackups ? $t('shell_dashboard.show_less') : $t('shell_dashboard.show_all', { n: backups.length }) }}
        </button>
        <div class="shell-notification mt-3" role="note" v-if="backups && backups.length">
          <i class="fas fa-circle-info"></i>
          <div>
            <strong>{{ $t('shell_dashboard.rollback_note_title') }}</strong>
            <span>{{ $t('shell_dashboard.rollback_note') }}</span>
          </div>
        </div>
      </template>
    </div>
  </section>
</template>

<script>
const POLL_MS = 10000;
const LIVE_POLL_MS = 2000;
const RECOVERY_WINDOW_MS = 60000;
// Shown as a per-minute rate only once the samples cover most of a minute
const RECOVERY_MIN_SPAN_MS = 50000;
const END_REASONS = ['disconnect', 'timeout', 'host', 'app_exit', 'client_quit'];
// The permissions pin.html shows as toggles, counted the same way: list is implied by view or
// launch and view by launch (pin.html shows them as on). File transfer bits have no toggle there.
const PERMISSIONS = [
  { bit: 0x01000000, impliedBy: 0x06000000 },  // list
  { bit: 0x02000000, impliedBy: 0x04000000 },  // view
  { bit: 0x04000000 },                          // launch
  { bit: 0x00010000 }, { bit: 0x00020000 },  // clipboard set/read
  { bit: 0x00040000 }, { bit: 0x00080000 },  // file upload/download
  { bit: 0x00100000 },                        // server commands
  { bit: 0x00000100 }, { bit: 0x00000200 }, { bit: 0x00000400 }, { bit: 0x00000800 }, { bit: 0x00001000 },  // inputs
];

async function getJson(url) {
  const r = await fetch(url, { credentials: 'include' });
  if (!r.ok) {
    throw new Error(`${url}: HTTP ${r.status}`);
  }
  return r.json();
}

function pad(n) {
  return String(n).padStart(2, '0');
}

export default {
  data() {
    return {
      version: '',
      platform: '',
      configName: '',
      appsHostName: '',
      apps: [],
      currentApp: '',
      appsFailed: false,
      clients: null,
      clientsFailed: false,
      backups: null,
      backupsFailed: false,
      showAllBackups: false,
      collapsedCount: 5,
      sessions: null,
      sessionsFailed: false,
      showAllSessions: false,
      sessionsCollapsedCount: 20,
      copyState: {},
      refreshing: false,
      updatedAt: null,
      timer: null,
      live: null,
      liveFailed: false,
      liveError: false,
      liveTimer: null,
      liveLoading: false,
      // uuid -> [{ t, n }] recovery request totals seen while polling, for the per-minute rate
      recoverySamples: {},
      disconnecting: '',
      disconnectError: '',
      // /api/shell/live "vdisplay" (Windows hosts); null on older hosts
      vdisplay: null,
      releasingVd: false,
      vdisplayError: false,
    };
  },
  computed: {
    hostName() {
      return this.configName || this.appsHostName;
    },
    runningApp() {
      if (!this.currentApp) {
        return '';
      }
      const current = String(this.currentApp);
      const app = this.apps.find(a => String(a.uuid) === current || String(a.id) === current);
      return app ? app.name : current;
    },
    connectedClients() {
      return (this.clients || []).filter(c => c.connected);
    },
    sortedClients() {
      return [...(this.clients || [])].sort((a, b) => (b.connected ? 1 : 0) - (a.connected ? 1 : 0));
    },
    visibleBackups() {
      if (!this.backups) {
        return [];
      }
      return this.showAllBackups ? this.backups : this.backups.slice(0, this.collapsedCount);
    },
    visibleSessions() {
      if (!this.sessions) {
        return [];
      }
      return this.showAllSessions ? this.sessions : this.sessions.slice(0, this.sessionsCollapsedCount);
    },
    lang() {
      return document.documentElement.getAttribute('lang') || undefined;
    },
    vdisplayHelper() {
      const vd = this.vdisplay;
      if (!vd) {
        return '';
      }
      if (vd.released) {
        return this.$t('shell_dashboard.vdisplay_released_desc');
      }
      if (!vd.active) {
        return this.$t('shell_dashboard.vdisplay_unused_desc');
      }
      if (vd.release_requested) {
        return this.$t('shell_dashboard.vdisplay_releasing');
      }
      if (vd.release_pending) {
        return this.$t('shell_dashboard.vdisplay_pending_desc');
      }
      const parts = [];
      if (vd.other_displays >= 0) {
        parts.push(vd.other_displays > 0
          ? this.$t('shell_dashboard.vdisplay_monitors', { n: vd.other_displays })
          : this.$t('shell_dashboard.vdisplay_no_monitors'));
      }
      parts.push(this.$t(vd.auto_release ? 'shell_dashboard.vdisplay_auto_on' : 'shell_dashboard.vdisplay_auto_off'));
      return parts.join(' \u00b7 ');
    },
  },
  mounted() {
    this.refresh(true);
    this.timer = setInterval(() => {
      if (!document.hidden) {
        this.refresh(false);
      }
    }, POLL_MS);
    this.loadLive();
    this.liveTimer = setInterval(() => {
      if (!document.hidden && !this.liveFailed) {
        this.loadLive();
      }
    }, LIVE_POLL_MS);
  },
  beforeUnmount() {
    clearInterval(this.timer);
    clearInterval(this.liveTimer);
  },
  methods: {
    async refresh(full) {
      if (this.refreshing) {
        return;
      }
      this.refreshing = true;
      const connectedBefore = this.clients ? this.connectedKey() : null;
      const tasks = [this.loadApps(), this.loadClients()];
      if (full) {
        tasks.push(this.loadConfig(), this.loadBackups(), this.loadSessions());
      }
      await Promise.allSettled(tasks);
      // The history only changes when a session ends, so reload it when the set of connected
      // devices changes (a count alone misses one session ending as another starts).
      if (!full && connectedBefore !== null && this.clients && this.connectedKey() !== connectedBefore) {
        await this.loadSessions();
      }
      this.updatedAt = new Date();
      this.refreshing = false;
    },
    async loadConfig() {
      try {
        const config = await getJson('./api/config');
        this.version = config.version || '';
        this.platform = config.platform || '';
        this.configName = config.shell_name || '';
      } catch (e) {
        console.error(e);
      }
    },
    async loadApps() {
      try {
        const r = await getJson('./api/apps');
        this.apps = Array.isArray(r.apps) ? r.apps : [];
        this.currentApp = r.current_app || '';
        this.appsHostName = r.host_name || '';
        this.appsFailed = false;
      } catch (e) {
        console.error(e);
        this.appsFailed = true;
      }
    },
    async loadClients() {
      try {
        const r = await getJson('./api/clients/list');
        if (!r.status) {
          throw new Error('clients/list: status false');
        }
        this.clients = (r.named_certs || []).map(({ name, uuid, connected, perm, display_mode }) => ({
          name, uuid, connected: !!connected, perm: parseInt(perm, 10) || 0, display_mode: display_mode || '',
        }));
        this.clientsFailed = false;
      } catch (e) {
        console.error(e);
        this.clientsFailed = true;
      }
    },
    async loadBackups() {
      // Older host builds have no such endpoint; show a quiet "unavailable" state then.
      try {
        const r = await getJson('./api/shell/backups');
        if (!r || r.status !== true || !Array.isArray(r.backups)) {
          throw new Error('shell/backups: unexpected response');
        }
        this.backups = r.backups;
        this.backupsFailed = false;
      } catch (e) {
        this.backups = null;
        this.backupsFailed = true;
      }
    },
    connectedKey() {
      return this.connectedClients.map(c => c.uuid || c.name).sort().join('|');
    },
    async loadSessions() {
      // Older host builds have no such endpoint; show a quiet "unavailable" state then. Once the
      // history has loaded, a later failed poll keeps the rows instead of claiming it is missing.
      try {
        const r = await getJson('./api/shell/sessions');
        if (!r || r.status !== true || !Array.isArray(r.sessions)) {
          throw new Error('shell/sessions: unexpected response');
        }
        this.sessions = r.sessions.filter(s => s && typeof s === 'object');
        this.sessionsFailed = false;
      } catch (e) {
        if (!Array.isArray(this.sessions)) {
          this.sessions = null;
          this.sessionsFailed = true;
        }
      }
    },
    async loadLive() {
      // Older host builds have no such endpoint: the panel stays hidden then.
      if (this.liveLoading) {
        return;
      }
      this.liveLoading = true;
      try {
        const r = await fetch('./api/shell/live', { credentials: 'include' });
        if (r.status === 404) {
          this.liveFailed = true;
          return;
        }
        if (!r.ok) {
          throw new Error(`shell/live: HTTP ${r.status}`);
        }
        const body = await r.json();
        if (!body || body.status !== true || !Array.isArray(body.sessions)) {
          throw new Error('shell/live: unexpected response');
        }
        const before = this.live ? this.live.map(s => this.liveKey(s)).sort().join('|') : null;
        this.live = body.sessions.filter(s => s && typeof s === 'object');
        this.vdisplay = body.vdisplay && typeof body.vdisplay === 'object' ? body.vdisplay : null;
        this.liveError = false;
        this.trackRecovery();
        // A stream started or ended (also a quick reconnect of the same device): the device list
        // and the history change with it
        if (before !== null && this.live.map(s => this.liveKey(s)).sort().join('|') !== before) {
          this.loadClients();
          this.loadSessions();
        }
      } catch (e) {
        console.error(e);
        this.liveError = true;
      } finally {
        this.liveLoading = false;
      }
    },
    liveKey(s) {
      // A new session of the same device starts its counters again
      return `${s.uuid || s.client}|${s.started}`;
    },
    trackRecovery() {
      const now = Date.now();
      const samples = {};
      for (const s of this.live) {
        const key = this.liveKey(s);
        const total = (Number(s.idr_requests) || 0) + (Number(s.ref_invalidations) || 0);
        const list = (this.recoverySamples[key] || []).filter(x => now - x.t <= RECOVERY_WINDOW_MS);
        list.push({ t: now, n: total });
        samples[key] = list;
      }
      this.recoverySamples = samples;
    },
    recoveryCount(s) {
      // Requests within the last minute of polling (the totals only grow during a session)
      const list = this.recoverySamples[this.liveKey(s)] || [];
      if (list.length < 2 || list[list.length - 1].t - list[0].t < RECOVERY_MIN_SPAN_MS) {
        return null;
      }
      return Math.max(0, list[list.length - 1].n - list[0].n);
    },
    recoveryPerMinute(s) {
      const n = this.recoveryCount(s);
      return n === null ? '-' : this.$t('shell_dashboard.recovery_per_min', { n });
    },
    recoveryLevel(s) {
      const n = this.recoveryCount(s);
      if (n === null) {
        return 'shell-status-off';
      }
      return n === 0 ? 'shell-status-ok' : n <= 3 ? 'shell-status-warn' : 'shell-status-bad';
    },
    recoveryDetail(s) {
      const parts = [this.$t('shell_dashboard.recovery_totals', {
        idr: Number(s.idr_requests) || 0, ref: Number(s.ref_invalidations) || 0,
      })];
      if (s.last_recovery_s !== undefined) {
        parts.push(this.$t('shell_dashboard.recovery_last', { time: this.formatDuration(s.last_recovery_s) }));
      }
      if (s.reported_frames_lost) {
        parts.push(this.$t('shell_dashboard.recovery_lost', { n: s.reported_frames_lost }));
      }
      return parts.join(' \u00b7 ');
    },
    liveBitrateDetail(s) {
      const parts = [this.$t('shell_dashboard.live_encoder', { rate: this.formatBitrate(s.encoder_kbps) })];
      const pacing = this.pacingDetail(s);
      if (pacing) {
        parts.push(pacing);
      }
      parts.push(this.$t(s.live_bitrate ? 'shell_dashboard.live_bitrate_on' : 'shell_dashboard.live_bitrate_off'));
      return parts.join(' \u00b7 ');
    },
    async releaseVirtualDisplay() {
      const streaming = (this.live || []).filter(s => s.state === 'running').length;
      if (streaming && !window.confirm(this.$t('shell_dashboard.vdisplay_release_confirm', { n: streaming }))) {
        return;
      }
      this.releasingVd = true;
      this.vdisplayError = false;
      let ok = false;
      try {
        const r = await fetch('./api/shell/display/release', {
          credentials: 'include',
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: '{}',
        });
        ok = r.ok && (await r.json()).status === true;
      } catch (e) {
        console.error(e);
      }
      this.vdisplayError = !ok;
      // The host releases it within a few seconds (after the streams end)
      setTimeout(() => {
        this.releasingVd = false;
        this.loadLive();
      }, 2500);
    },
    async disconnectLive(s) {
      if (!window.confirm(this.$t('shell_dashboard.live_disconnect_confirm', { name: s.client || s.uuid }))) {
        return;
      }
      this.disconnecting = s.uuid;
      this.disconnectError = '';
      let ok = false;
      try {
        const r = await fetch('./api/clients/disconnect', {
          credentials: 'include',
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ uuid: s.uuid }),
        });
        ok = r.ok && (await r.json()).status === true;
      } catch (e) {
        console.error(e);
      }
      if (!ok) {
        this.disconnectError = s.uuid;
      }
      this.disconnecting = '';
      setTimeout(() => this.loadLive(), 500);
    },
    permissionSummary(perm) {
      const granted = PERMISSIONS.filter(p => (perm & (p.bit | (p.impliedBy || 0))) !== 0).length;
      if (granted === PERMISSIONS.length) {
        return this.$t('shell_dashboard.perm_all');
      }
      if (granted === 0) {
        return this.$t('shell_dashboard.perm_none');
      }
      return this.$t('shell_dashboard.perm_some', { n: granted, total: PERMISSIONS.length });
    },
    rollbackCommand(backup) {
      const path = String(backup.path || '').replace(/"/g, '');
      return `powershell -ExecutionPolicy Bypass -File .\\Install-Shell.ps1 -Rollback -BackupPath "${path}"`;
    },
    async copyRollback(backup) {
      const text = this.rollbackCommand(backup);
      let ok = false;
      try {
        if (navigator.clipboard && window.isSecureContext) {
          await navigator.clipboard.writeText(text);
          ok = true;
        }
      } catch (e) {
        ok = false;
      }
      if (!ok) {
        ok = this.copyWithSelection(text);
      }
      this.copyState = { ...this.copyState, [backup.name]: ok ? 'ok' : 'failed' };
      if (ok) {
        setTimeout(() => {
          if (this.copyState[backup.name] === 'ok') {
            this.copyState = { ...this.copyState, [backup.name]: undefined };
          }
        }, 2500);
      }
    },
    copyWithSelection(text) {
      const area = document.createElement('textarea');
      area.value = text;
      area.setAttribute('readonly', '');
      area.style.position = 'fixed';
      area.style.opacity = '0';
      document.body.appendChild(area);
      area.select();
      let ok = false;
      try {
        ok = document.execCommand('copy');
      } catch (e) {
        ok = false;
      }
      document.body.removeChild(area);
      return ok;
    },
    parseLocal(value) {
      // "yyyy-MM-ddTHH:mm:ss" without an offset is read as local time.
      if (!value) {
        return null;
      }
      const d = new Date(value);
      return isNaN(d.getTime()) ? null : d;
    },
    formatStamp(value) {
      const d = this.parseLocal(value);
      if (!d) {
        return '';
      }
      return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
    },
    formatDuration(seconds) {
      const total = Math.max(0, Math.round(Number(seconds)));
      if (!isFinite(total)) {
        return '-';
      }
      const h = Math.floor(total / 3600);
      const m = Math.floor((total % 3600) / 60);
      const s = total % 60;
      return h > 0 ? `${h}:${pad(m)}:${pad(s)}` : `${m}:${pad(s)}`;
    },
    formatVideo(session) {
      const size = session.width && session.height ? `${session.width}\u00d7${session.height}` : '';
      const fps = session.fps ? `${session.fps} fps` : '';
      return [size, fps].filter(Boolean).join(' \u00b7 ') || '-';
    },
    videoDetail(session) {
      const parts = [];
      if (session.codec) {
        parts.push(session.codec);
      }
      if (session.hdr) {
        parts.push(this.$t('shell_dashboard.hdr_requested'));
      }
      return parts.join(' \u00b7 ');
    },
    formatBitrate(kbps) {
      const value = Number(kbps);
      if (!value || value <= 0) {
        return '-';
      }
      if (value < 1000) {
        return `${Math.round(value)} kbps`;
      }
      const mbps = value / 1000;
      return `${mbps >= 10 ? Math.round(mbps) : Math.round(mbps * 10) / 10} Mbps`;
    },
    pacingDetail(session) {
      const parts = [];
      if (session.pacing_mbps) {
        const key = session.pacing_auto ? 'shell_dashboard.pacing_auto' : 'shell_dashboard.pacing';
        parts.push(this.$t(key, { rate: session.pacing_mbps }));
      }
      if (session.fec_percent !== undefined && session.fec_percent !== null) {
        parts.push(`FEC ${session.fec_percent}%`);
      }
      return parts.join(' \u00b7 ');
    },
    endReason(reason) {
      return END_REASONS.includes(reason) ? this.$t(`shell_dashboard.end_${reason}`) : String(reason);
    },
    endReasonDetail(reason) {
      return reason === 'disconnect' || reason === 'timeout' ? this.$t(`shell_dashboard.end_${reason}_desc`) : '';
    },
    formatClock(d) {
      return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
    },
    relativeTime(value) {
      const d = this.parseLocal(value);
      if (!d || typeof Intl.RelativeTimeFormat !== 'function') {
        return '';
      }
      const seconds = Math.round((d.getTime() - Date.now()) / 1000);
      const units = [['year', 31536000], ['month', 2592000], ['day', 86400], ['hour', 3600], ['minute', 60]];
      const rtf = new Intl.RelativeTimeFormat(this.lang, { numeric: 'auto' });
      for (const [unit, size] of units) {
        if (Math.abs(seconds) >= size) {
          return rtf.format(Math.round(seconds / size), unit);
        }
      }
      return rtf.format(0, 'minute');
    },
  },
};
</script>

<style scoped>
.shell-dash {
  margin: 24px 0 32px;
}

.shell-section-head {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 16px;
}

.shell-helper {
  font-size: 12px;
  line-height: 16px;
  letter-spacing: .32px;
  color: var(--shell-text-helper);
}

.shell-icon-btn {
  width: 32px;
  height: 32px;
  padding: 0;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  color: var(--shell-text-secondary);
}

.shell-icon-btn:hover {
  color: var(--shell-text);
}

.shell-num {
  font-variant-numeric: tabular-nums;
}

/* Summary strip: key/value cells separated by 1px rules. */
.shell-summary {
  display: grid;
  grid-template-columns: repeat(6, minmax(0, 1fr));
  margin: 0 0 24px;
  background: var(--shell-layer);
  border: 1px solid var(--shell-border-subtle);
  border-radius: 4px;
}

.shell-kv {
  padding: 12px 16px;
  min-width: 0;
  border-left: 1px solid var(--shell-border-subtle);
}

.shell-kv:first-child {
  border-left: 0;
}

.shell-kv dt {
  margin-bottom: 4px;
}

.shell-kv dd {
  margin: 0;
  font-size: 14px;
  line-height: 20px;
  display: flex;
  align-items: center;
  min-width: 0;
}

.shell-kv dd.text-truncate {
  display: block;
}

@media (max-width: 991.98px) {
  .shell-summary {
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }

  .shell-kv:nth-child(3n + 1) {
    border-left: 0;
  }

  .shell-kv:nth-child(n + 4) {
    border-top: 1px solid var(--shell-border-subtle);
  }
}

@media (max-width: 575.98px) {
  .shell-summary {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .shell-kv:nth-child(n) {
    border-left: 1px solid var(--shell-border-subtle);
    border-top: 1px solid var(--shell-border-subtle);
  }

  .shell-kv:nth-child(odd) {
    border-left: 0;
  }

  .shell-kv:nth-child(-n + 2) {
    border-top: 0;
  }
}

/* Panels with a header row and a data table. */
.shell-panel {
  background: var(--shell-layer);
  border: 1px solid var(--shell-border-subtle);
  border-radius: 4px;
  padding: 16px;
  margin-bottom: 24px;
}

.shell-panel-head {
  display: flex;
  align-items: baseline;
  flex-wrap: wrap;
  gap: 8px 12px;
  margin-bottom: 12px;
}

.shell-panel-desc {
  margin: -4px 0 12px;
}

.shell-panel-link {
  font-size: 14px;
  text-decoration: none;
}

.shell-panel-link:hover {
  text-decoration: underline;
}

.shell-table-wrap {
  border: 1px solid var(--shell-border-subtle);
}

.shell-table {
  min-width: 560px;
}

.shell-name-cell {
  font-weight: 500;
}

.shell-status {
  display: inline-block;
  width: 8px;
  height: 8px;
  margin-right: 8px;
  flex-shrink: 0;
  vertical-align: 1px;
}

.shell-status-ok { background: var(--shell-success); }
.shell-status-warn { background: var(--shell-warning); }
.shell-status-bad { background: var(--shell-danger, #da1e28); }
.shell-status-off { background: transparent; box-shadow: inset 0 0 0 1px var(--shell-border-strong); }

.shell-command-row td {
  background: var(--shell-layer-2);
}

.shell-command {
  display: block;
  padding: 8px 12px;
  background: var(--shell-field);
  border-bottom: 1px solid var(--shell-border-strong);
  font-size: 12px;
  line-height: 16px;
  user-select: all;
}

.shell-vd-row {
  display: flex;
  align-items: center;
  flex-wrap: wrap;
  gap: 12px 16px;
}

.shell-vd-state {
  flex: 1 1 280px;
  min-width: 0;
  font-size: 14px;
  line-height: 20px;
}

/* Carbon-style inline notification: neutral surface, 3px info bar. */
.shell-notification {
  display: flex;
  gap: 12px;
  align-items: flex-start;
  padding: 12px 16px;
  background: var(--shell-layer-2);
  border: 1px solid var(--shell-border-subtle);
  border-left: 3px solid var(--shell-info);
  color: var(--shell-text);
  font-size: 14px;
  line-height: 20px;
}

.shell-notification i {
  color: var(--shell-info);
  font-size: 16px;
  margin-top: 2px;
}

.shell-notification strong {
  font-weight: 600;
  margin-right: 4px;
}
</style>
