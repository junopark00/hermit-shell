<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config'
])

const defaultGameStreamPort = 47989

const config = ref(props.config)

const effectivePort = computed(() => +config.value?.port ?? defaultGameStreamPort)

// Webhooks (docs/features.md "Webhooks"): the URL and the options are ordinary settings; the
// secret is written through its own API and never read back; a test event is queued and its
// result read from GET /api/shell/webhook.
const webhookGroups = ['pairing', 'stream', 'app']

const webhookEventList = computed(() =>
  String(config.value.webhook_events ?? '').split(',').map((s) => s.trim().toLowerCase()).filter((s) => s))

const webhookEvent = (group) => computed({
  get: () => webhookEventList.value.includes(group),
  set: (on) => {
    const list = webhookEventList.value.filter((g) => g !== group)

    if (on) list.push(group)
    const selected = webhookGroups.filter((g) => list.includes(g))
    // An empty value is not written to shell.conf (the default would come back): `none` means no group
    config.value.webhook_events = selected.length ? selected.join(',') : 'none'
  },
})

const webhookEventModels = Object.fromEntries(webhookGroups.map((g) => [g, webhookEvent(g)]))

const webhookStatus = ref(null)

const webhookSecret = ref('')

const webhookSecretMessage = ref('')

const webhookTestMessage = ref('')

let webhookPoll = null

async function loadWebhookStatus() {
  try {
    const r = await fetch('./api/shell/webhook', { credentials: 'include' })

    if (r.ok) webhookStatus.value = await r.json()
  } catch (e) {
    console.error(e)
  }
}

function stopWebhookPoll() {
  if (webhookPoll) {
    clearInterval(webhookPoll)
    webhookPoll = null
  }
}

async function saveWebhookSecret(clear) {
  webhookSecretMessage.value = ''

  try {
    const r = await fetch('./api/shell/webhook/secret', {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ secret: clear ? '' : webhookSecret.value }),
    })

    const body = await r.json().catch(() => ({}))

    if (r.ok && body.status === true) {
      webhookSecret.value = ''
      webhookSecretMessage.value = clear ? 'webhook_secret_cleared' : 'webhook_secret_saved'
      await loadWebhookStatus()
    } else {
      webhookSecretMessage.value = 'webhook_secret_error'
    }
  } catch (e) {
    console.error(e)
    webhookSecretMessage.value = 'webhook_secret_error'
  }
}

async function sendWebhookTest() {
  webhookTestMessage.value = ''
  stopWebhookPoll()
  const before = webhookStatus.value?.last?.time ?? null

  try {
    const r = await fetch('./api/shell/webhook/test', {
      credentials: 'include',
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}',
    })

    const body = await r.json().catch(() => ({}))

    if (!r.ok || body.status !== true) {
      webhookTestMessage.value = body.error || 'webhook_test_error'

      return
    }
  } catch (e) {
    console.error(e)
    webhookTestMessage.value = 'webhook_test_error'

    return
  }

  webhookTestMessage.value = 'webhook_test_queued'
  // The result arrives when the delivery thread is done (up to about 40 s with retries)
  let polls = 0
  webhookPoll = setInterval(async () => {
    await loadWebhookStatus()
    const last = webhookStatus.value?.last

    if ((last && last.time !== before) || ++polls >= 25) {
      stopWebhookPoll()
      webhookTestMessage.value = ''
    }
  }, 2000)
}

onMounted(loadWebhookStatus)

onBeforeUnmount(stopWebhookPoll)
</script>

<template>
  <div id="network" class="config-page">
    <!-- UPnP -->
    <Checkbox class="mb-3"
              id="upnp"
              locale-prefix="config"
              v-model="config.upnp"
              default="false"
    ></Checkbox>

    <!-- Address family -->
    <div class="mb-3">
      <label for="address_family" class="form-label">{{ $t('config.address_family') }}</label>
      <select id="address_family" class="form-select" v-model="config.address_family">
        <option value="ipv4">{{ $t('config.address_family_ipv4') }}</option>
        <option value="both">{{ $t('config.address_family_both') }}</option>
      </select>
      <div class="form-text">{{ $t('config.address_family_desc') }}</div>
    </div>

    <!-- Port family -->
    <div class="mb-3">
      <label for="port" class="form-label">{{ $t('config.port') }}</label>
      <input type="number" min="1029" max="65514" class="form-control" id="port" :placeholder="defaultGameStreamPort"
             v-model="config.port" />
      <div class="form-text">{{ $t('config.port_desc') }}</div>
      <!-- Add warning if any port is less than 1024 -->
      <div class="alert alert-danger" v-if="(+effectivePort - 5) < 1024">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_1') }}
      </div>
      <!-- Add warning if any port is above 65535 -->
      <div class="alert alert-danger" v-if="(+effectivePort + 21) > 65535">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_alert_2') }}
      </div>
      <!-- Create a port table for the various ports needed by Shell -->
      <table class="table">
        <thead>
        <tr>
          <th scope="col">{{ $t('config.port_protocol') }}</th>
          <th scope="col">{{ $t('config.port_port') }}</th>
          <th scope="col">{{ $t('config.port_note') }}</th>
        </tr>
        </thead>
        <tbody>
        <tr>
          <!-- HTTPS -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort - 5}}</td>
          <td></td>
        </tr>
        <tr>
          <!-- HTTP -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort}}</td>
          <td>
            <div class="alert alert-primary" role="alert" v-if="+effectivePort !== defaultGameStreamPort">
              <i class="fa-solid fa-xl fa-circle-info"></i> {{ $t('config.port_http_port_note') }}
            </div>
          </td>
        </tr>
        <tr>
          <!-- Web UI -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort + 1}}</td>
          <td>{{ $t('config.port_web_ui') }}</td>
        </tr>
        <tr>
          <!-- RTSP -->
          <td>{{ $t('config.port_tcp') }}</td>
          <td>{{+effectivePort + 21}}</td>
          <td></td>
        </tr>
        <tr>
          <!-- Video, Control, Audio -->
          <td>{{ $t('config.port_udp') }}</td>
          <td>{{+effectivePort + 9}} - {{+effectivePort + 11}}</td>
          <td></td>
        </tr>
        <!--            <tr>-->
        <!--              &lt;!&ndash; Mic &ndash;&gt;-->
        <!--              <td>UDP</td>-->
        <!--              <td>{{+effectivePort + 13}}</td>-->
        <!--              <td></td>-->
        <!--            </tr>-->
        </tbody>
      </table>
      <!-- add warning about exposing web ui to the internet -->
      <div class="alert alert-warning" v-if="config.origin_web_ui_allowed === 'wan'">
        <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.port_warning') }}
      </div>
    </div>

    <!-- Origin Web UI Allowed -->
    <div class="mb-3">
      <label for="origin_web_ui_allowed" class="form-label">{{ $t('config.origin_web_ui_allowed') }}</label>
      <select id="origin_web_ui_allowed" class="form-select" v-model="config.origin_web_ui_allowed">
        <option value="pc">{{ $t('config.origin_web_ui_allowed_pc') }}</option>
        <option value="lan">{{ $t('config.origin_web_ui_allowed_lan') }}</option>
        <option value="wan">{{ $t('config.origin_web_ui_allowed_wan') }}</option>
      </select>
      <div class="form-text">{{ $t('config.origin_web_ui_allowed_desc') }}</div>
    </div>

    <!-- External IP -->
    <div class="mb-3">
      <label for="external_ip" class="form-label">{{ $t('config.external_ip') }}</label>
      <input type="text" class="form-control" id="external_ip" placeholder="123.456.789.12" v-model="config.external_ip" />
      <div class="form-text">{{ $t('config.external_ip_desc') }}</div>
    </div>

    <!-- LAN Encryption Mode -->
    <div class="mb-3">
      <label for="lan_encryption_mode" class="form-label">{{ $t('config.lan_encryption_mode') }}</label>
      <select id="lan_encryption_mode" class="form-select" v-model="config.lan_encryption_mode">
        <option value="0">{{ $t('_common.disabled_def') }}</option>
        <option value="1">{{ $t('config.lan_encryption_mode_1') }}</option>
        <option value="2">{{ $t('config.lan_encryption_mode_2') }}</option>
      </select>
      <div class="form-text">{{ $t('config.lan_encryption_mode_desc') }}</div>
    </div>

    <!-- WAN Encryption Mode -->
    <div class="mb-3">
      <label for="wan_encryption_mode" class="form-label">{{ $t('config.wan_encryption_mode') }}</label>
      <select id="wan_encryption_mode" class="form-select" v-model="config.wan_encryption_mode">
        <option value="0">{{ $t('_common.disabled') }}</option>
        <option value="1">{{ $t('config.wan_encryption_mode_1') }}</option>
        <option value="2">{{ $t('config.wan_encryption_mode_2') }}</option>
      </select>
      <div class="form-text">{{ $t('config.wan_encryption_mode_desc') }}</div>
    </div>

    <!-- Ping Timeout -->
    <div class="mb-3">
      <label for="ping_timeout" class="form-label">{{ $t('config.ping_timeout') }}</label>
      <input type="text" class="form-control" id="ping_timeout" placeholder="10000" v-model="config.ping_timeout" />
      <div class="form-text">{{ $t('config.ping_timeout_desc') }}</div>
    </div>

    <!-- Webhooks (Shell) -->
    <div class="mb-3 border rounded p-3">
      <h5>{{ $t('config.webhook_title') }}</h5>
      <div class="form-text mb-3">{{ $t('config.webhook_desc') }}</div>

      <div class="mb-3">
        <label for="webhook_url" class="form-label">{{ $t('config.webhook_url') }}</label>
        <input type="text" class="form-control" id="webhook_url" placeholder="https://" v-model="config.webhook_url" />
        <div class="form-text">{{ $t('config.webhook_url_desc') }}</div>
        <div class="alert alert-warning mt-2" v-if="webhookStatus && webhookStatus.url_error">
          <i class="fa-solid fa-xl fa-triangle-exclamation"></i> {{ $t('config.webhook_url_error') }} {{ webhookStatus.url_error }}
        </div>
      </div>

      <div class="mb-3">
        <label class="form-label">{{ $t('config.webhook_events') }}</label>
        <div class="form-check" v-for="group in webhookGroups" :key="group">
          <input type="checkbox" class="form-check-input" :id="'webhook_event_' + group" v-model="webhookEventModels[group].value" />
          <label class="form-check-label" :for="'webhook_event_' + group">{{ $t('config.webhook_event_' + group) }}</label>
        </div>
        <div class="form-text">{{ $t('config.webhook_events_desc') }}</div>
      </div>

      <Checkbox class="mb-3"
                id="webhook_include_client_ip"
                locale-prefix="config"
                v-model="config.webhook_include_client_ip"
                default="false"
      ></Checkbox>

      <Checkbox class="mb-3"
                id="webhook_allow_http_private"
                locale-prefix="config"
                v-model="config.webhook_allow_http_private"
                default="false"
      ></Checkbox>

      <div class="mb-3">
        <label for="webhook_secret" class="form-label">
          {{ $t('config.webhook_secret') }}
          <span class="badge" :class="webhookStatus && webhookStatus.secret_set ? 'bg-success' : 'bg-secondary'" v-if="webhookStatus">
            {{ webhookStatus.secret_set ? $t('config.webhook_secret_set') : $t('config.webhook_secret_not_set') }}
          </span>
        </label>
        <div class="input-group">
          <!-- The real value is cleared after a save (the page never keeps the secret); masked dots
               stand in as a placeholder while the host reports a secret -->
          <input type="password" class="form-control" id="webhook_secret" autocomplete="new-password" v-model="webhookSecret"
                 :placeholder="webhookStatus && webhookStatus.secret_set ? '••••••••••••••••' : ''" />
          <button class="btn btn-primary" type="button" :disabled="!webhookSecret" @click="saveWebhookSecret(false)">{{ $t('config.webhook_secret_save') }}</button>
          <button class="btn btn-danger" type="button" :disabled="!(webhookStatus && webhookStatus.secret_set)" @click="saveWebhookSecret(true)">{{ $t('config.webhook_secret_clear') }}</button>
        </div>
        <div class="form-text">{{ $t('config.webhook_secret_desc') }}</div>
        <div class="form-text" v-if="webhookSecretMessage">{{ $t('config.' + webhookSecretMessage) }}</div>
      </div>

      <div class="mb-1">
        <button class="btn btn-primary" type="button" :disabled="!(webhookStatus && webhookStatus.enabled)" @click="sendWebhookTest">{{ $t('config.webhook_test') }}</button>
        <span class="ms-2 form-text" v-if="webhookTestMessage">{{ webhookTestMessage.startsWith('webhook_') ? $t('config.' + webhookTestMessage) : webhookTestMessage }}</span>
        <div class="form-text" v-if="webhookStatus && !webhookStatus.enabled && !webhookStatus.url_error">{{ $t('config.webhook_test_off') }}</div>
        <div class="form-text" v-if="webhookStatus && webhookStatus.last">
          {{ $t('config.webhook_last') }} {{ webhookStatus.last.event }} {{ webhookStatus.last.time }}:
          <span :class="webhookStatus.last.ok ? 'text-success' : 'text-danger'">
            {{ webhookStatus.last.ok ? $t('config.webhook_last_ok') : $t('config.webhook_last_failed') }}
            {{ webhookStatus.last.ok ? ('HTTP ' + webhookStatus.last.http_code) : webhookStatus.last.error }}
          </span>
          ({{ $t('config.webhook_attempts') }} {{ webhookStatus.last.attempts }})
        </div>
        <div class="form-text" v-if="webhookStatus && webhookStatus.enabled">
          {{ $t('config.webhook_counters') }} {{ webhookStatus.sent }} / {{ webhookStatus.failed }} / {{ webhookStatus.dropped }} / {{ webhookStatus.queued }}
        </div>
      </div>
    </div>

  </div>
</template>

<style scoped>

</style>
