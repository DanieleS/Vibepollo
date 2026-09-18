<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useI18n } from 'vue-i18n';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';

interface IgdbStatus {
  enabled?: boolean;
  configured?: boolean;
  authenticated?: boolean;
  last_error?: string;
}

const { t } = useI18n();
const store = useConfigStore();
const config = store.config;

const status = ref<IgdbStatus | null>(null);
// Write-only: the host never returns the stored secret, only whether it holds one.
const secret = ref('');
const busy = ref('');
const message = ref('');
const failed = ref(false);

const configured = computed(() => status.value?.configured === true);

function note(text: string, bad = false) {
  message.value = text;
  failed.value = bad;
}

async function refreshStatus() {
  try {
    status.value = (await http.get('/api/igdb/status')).data;
  } catch {
    status.value = null;
  }
}

async function saveSecret(value: string) {
  busy.value = 'secret';
  try {
    await http.post('/api/igdb/secret', { secret: value });
    secret.value = '';
    await refreshStatus();
    note(value ? 'Client secret stored.' : 'Client secret removed.');
  } catch {
    note('The client secret could not be stored.', true);
  } finally {
    busy.value = '';
  }
}

async function verify() {
  busy.value = 'verify';
  try {
    const result = (await http.post('/api/igdb/verify')).data;
    await refreshStatus();
    note(
      result?.status ? 'IGDB accepted these credentials.' : result?.error || 'IGDB rejected these credentials.',
      !result?.status,
    );
  } catch {
    note('IGDB rejected these credentials.', true);
  } finally {
    busy.value = '';
  }
}

async function resolveLibrary() {
  busy.value = 'resolve';
  try {
    // The host queues the pass rather than running it inside the request: a few hundred games
    // take minutes at IGDB's four requests a second.
    await http.post('/api/igdb/resolve', { force: false });
    await refreshStatus();
    note(t('metadata.resolve_started'));
  } catch {
    note('The library pass could not run.', true);
  } finally {
    busy.value = '';
  }
}

onMounted(() => {
  void refreshStatus();
});
</script>

<template>
  <div id="metadata" class="config-page space-y-5">
    <div
      class="rounded-xl border border-dark/10 bg-light/60 px-4 py-3 dark:border-light/10 dark:bg-dark/40 sm:px-5 sm:py-4"
    >
      <div class="flex items-start gap-3">
        <span
          class="mt-0.5 inline-flex h-8 w-8 shrink-0 items-center justify-center rounded-lg bg-primary/10 text-primary"
        >
          <i class="fas fa-book-open text-sm" />
        </span>
        <div class="min-w-0">
          <h3 class="text-sm font-semibold leading-tight">{{ $t('metadata.config_header') }}</h3>
          <p class="mt-1 text-xs leading-relaxed opacity-70">
            {{ $t('metadata.config_desc') }}
          </p>
        </div>
      </div>
    </div>

    <ConfigFieldRenderer v-model="config.igdb_enabled" setting-key="igdb_enabled" class="mb-3" />

    <template v-if="config.igdb_enabled">
      <ConfigFieldRenderer
        v-model="config.igdb_client_id"
        setting-key="igdb_client_id"
        class="mb-3"
      />

      <div class="mb-3">
        <n-form-item :label="$t('metadata.secret_label')">
          <div class="flex flex-wrap items-center gap-2 w-full">
            <n-input
              id="igdb-secret"
              v-model:value="secret"
              type="password"
              show-password-on="click"
              class="flex-1 min-w-[12rem]"
              :placeholder="$t('metadata.secret_label')"
            />
            <n-button
              size="small"
              type="primary"
              strong
              :disabled="!secret || busy === 'secret'"
              @click="saveSecret(secret)"
            >
              {{ $t('_common.save') }}
            </n-button>
            <n-button
              size="small"
              secondary
              :disabled="!configured || busy === 'secret'"
              @click="saveSecret('')"
            >
              {{ $t('_common.remove') }}
            </n-button>
          </div>
        </n-form-item>
        <n-text depth="3" class="text-xs">
          {{ configured ? $t('metadata.secret_stored') : $t('metadata.secret_desc') }}
        </n-text>
      </div>

      <ConfigFieldRenderer
        v-model="config.igdb_auto_resolve"
        setting-key="igdb_auto_resolve"
        class="mb-3"
      />
      <ConfigFieldRenderer
        v-model="config.igdb_allow_name_match"
        setting-key="igdb_allow_name_match"
        class="mb-3"
      />
      <ConfigFieldRenderer
        v-model="config.igdb_cache_ttl_days"
        setting-key="igdb_cache_ttl_days"
        class="mb-3"
      />

      <div class="flex flex-wrap items-center gap-2">
        <n-button
          size="small"
          type="primary"
          strong
          :disabled="!configured || busy === 'verify'"
          @click="verify"
        >
          {{ $t('metadata.verify') }}
        </n-button>
        <n-button
          size="small"
          secondary
          :disabled="!configured || busy === 'resolve'"
          @click="resolveLibrary"
        >
          {{ $t('metadata.resolve') }}
        </n-button>
      </div>

      <div v-if="message" class="mt-3 text-sm" :class="failed ? 'text-danger' : 'opacity-70'">
        {{ message }}
      </div>
      <div v-else-if="status?.last_error" class="mt-3 text-sm text-danger">
        {{ status.last_error }}
      </div>
    </template>
  </div>
</template>

<style scoped></style>
