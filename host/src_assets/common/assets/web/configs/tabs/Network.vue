<script setup>
import { computed, ref } from 'vue'
import Checkbox from "../../Checkbox.vue";

const props = defineProps([
  'platform',
  'config'
])

const defaultMoonlightPort = 47989

const config = ref(props.config)
const effectivePort = computed(() => +config.value?.port ?? defaultMoonlightPort)
const isOff = (v) => [false, 0, 'false', '0', 'disabled', 'disable', 'no', 'off'].includes(typeof v === 'string' ? v.trim().toLowerCase() : v)
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
      <input type="number" min="1029" max="65514" class="form-control" id="port" :placeholder="defaultMoonlightPort"
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
      <!-- Create a port table for the various ports needed by NyxPlay Host -->
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
            <div class="alert alert-primary" role="alert" v-if="+effectivePort !== defaultMoonlightPort">
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

    <!-- Unencrypted RTSP -->
    <div class="mb-3">
      <label for="rtsp_plaintext" class="form-label">{{ $t('config.rtsp_plaintext') }}</label>
      <select id="rtsp_plaintext" class="form-select" v-model="config.rtsp_plaintext">
        <option value="0">{{ $t('config.rtsp_plaintext_0') }}</option>
        <option value="1">{{ $t('config.rtsp_plaintext_1') }}</option>
        <option value="2">{{ $t('config.rtsp_plaintext_2') }}</option>
      </select>
      <div class="form-text">{{ $t('config.rtsp_plaintext_desc') }}</div>
    </div>

    <!-- Ping Timeout -->
    <div class="mb-3">
      <label for="ping_timeout" class="form-label">{{ $t('config.ping_timeout') }}</label>
      <input type="text" class="form-control" id="ping_timeout" placeholder="10000" v-model="config.ping_timeout" />
      <div class="form-text">{{ $t('config.ping_timeout_desc') }}</div>
    </div>

    <!-- Security event log and automatic blocking -->
    <hr class="my-4">
    <h5 class="mb-3">
      {{ $t('security.title') }}
      <a class="btn btn-sm btn-outline-secondary ms-2" href="./security">{{ $t('navbar.security') }}</a>
    </h5>

    <!-- Security log -->
    <Checkbox class="mb-3"
              id="security_log"
              locale-prefix="config"
              v-model="config.security_log"
              default="true"
    ></Checkbox>

    <!-- Security log size limit -->
    <div class="mb-3" v-if="!isOff(config.security_log)">
      <label for="security_log_max_mb" class="form-label">{{ $t('config.security_log_max_mb') }}</label>
      <input type="number" min="1" max="100" class="form-control" id="security_log_max_mb" placeholder="10"
             v-model="config.security_log_max_mb" />
      <div class="form-text">{{ $t('config.security_log_max_mb_desc') }}</div>
    </div>

    <!-- Automatic blocking -->
    <Checkbox class="mb-3"
              id="ban_enabled"
              locale-prefix="config"
              v-model="config.ban_enabled"
              default="true"
    ></Checkbox>

    <div v-if="!isOff(config.ban_enabled)">
      <!-- Failures before blocking -->
      <div class="mb-3">
        <label for="ban_threshold" class="form-label">{{ $t('config.ban_threshold') }}</label>
        <input type="number" min="3" max="1000" class="form-control" id="ban_threshold" placeholder="10"
               v-model="config.ban_threshold" />
        <div class="form-text">{{ $t('config.ban_threshold_desc') }}</div>
      </div>

      <!-- Failure window -->
      <div class="mb-3">
        <label for="ban_window_seconds" class="form-label">{{ $t('config.ban_window_seconds') }}</label>
        <input type="number" min="10" max="86400" class="form-control" id="ban_window_seconds" placeholder="600"
               v-model="config.ban_window_seconds" />
        <div class="form-text">{{ $t('config.ban_window_seconds_desc') }}</div>
      </div>

      <!-- Block duration -->
      <div class="mb-3">
        <label for="ban_duration_seconds" class="form-label">{{ $t('config.ban_duration_seconds') }}</label>
        <input type="number" min="0" max="2592000" class="form-control" id="ban_duration_seconds" placeholder="1800"
               v-model="config.ban_duration_seconds" />
        <div class="form-text">{{ $t('config.ban_duration_seconds_desc') }}</div>
      </div>

      <!-- Never-block list -->
      <div class="mb-3">
        <label for="ban_whitelist" class="form-label">{{ $t('config.ban_whitelist') }}</label>
        <input type="text" class="form-control" id="ban_whitelist" placeholder="192.168.1.20, 10.0.0.0/24"
               v-model="config.ban_whitelist" />
        <div class="form-text">{{ $t('config.ban_whitelist_desc') }}</div>
      </div>
    </div>

  </div>
</template>

<style scoped>

</style>
