<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { apiDelete, apiGet, apiPatch, apiPost } from '@/api/client';
import { AppButton, InlineAlert, SettingRow, StatusBadge } from '@/components/ui';
import { configBoolean } from '@/utils/settings';

interface IgdbStatus {
  enabled?: boolean;
  configured?: boolean;
  authenticated?: boolean;
  last_error?: string;
  client_id?: string;
  resolving?: boolean;
  secret_file?: string;
}

// Playnite only exists on Windows; the switch that governs what it writes is meaningless elsewhere.
const props = withDefaults(defineProps<{ platform?: string }>(), { platform: '' });
const isWindows = computed(() => props.platform.toLocaleLowerCase().includes('windows'));

const { t } = useI18n();

const status = ref<IgdbStatus | null>(null);
const values = ref<Record<string, unknown>>({});
const original = ref<Record<string, unknown>>({});
// Never read back from the host: the secret leaves the browser once and is not returned.
const secret = ref('');

const loading = ref(true);
const saving = ref(false);
const busy = ref('');
const failed = ref(false);
const message = ref('');
const messageTone = ref<'success' | 'warning'>('success');

// A typed secret is an unsaved change like any other. It travels to its own endpoint rather
// than through the config, but that is where it is stored, not something to make the user
// think about: one Save covers the whole credential.
const dirty = computed(
  () => JSON.stringify(values.value) !== JSON.stringify(original.value) || secret.value !== '',
);
const enabled = computed(() => values.value.igdb_enabled === true);
const ready = computed(() => status.value?.configured === true);

const statusLabel = computed(() => {
  if (!status.value) return t('ui.integrations.status.unavailable');
  if (!ready.value) return t('ui.metadata.status.notConfigured');
  if (status.value.last_error) return t('ui.metadata.status.failing');
  if (status.value.authenticated) return t('ui.metadata.status.connected');
  return t('ui.metadata.status.configured');
});

const statusTone = computed(() => {
  if (!ready.value) return 'neutral' as const;
  if (status.value?.last_error) return 'warning' as const;
  return status.value?.authenticated ? ('success' as const) : ('info' as const);
});

function note(text: string, tone: 'success' | 'warning' = 'success') {
  message.value = text;
  messageTone.value = tone;
}

async function load() {
  loading.value = true;
  failed.value = false;
  try {
    const [config, igdb] = await Promise.all([
      apiGet<Record<string, unknown>>('/api/config'),
      apiGet<IgdbStatus>('/api/igdb/status'),
    ]);
    const next: Record<string, unknown> = {
      igdb_enabled: configBoolean(config.igdb_enabled ?? false),
      igdb_client_id: (config.igdb_client_id as string) ?? '',
      igdb_auto_resolve: configBoolean(config.igdb_auto_resolve ?? true),
      igdb_allow_name_match: configBoolean(config.igdb_allow_name_match ?? true),
      igdb_cache_ttl_days: Number(config.igdb_cache_ttl_days ?? 30),
    };
    if (isWindows.value) {
      next.playnite_sync_metadata = configBoolean(config.playnite_sync_metadata ?? true, true);
    }
    values.value = next;
    original.value = JSON.parse(JSON.stringify(next));
    status.value = igdb;
  } catch {
    failed.value = true;
  } finally {
    loading.value = false;
  }
}

async function save() {
  if (saving.value || !dirty.value) return;
  saving.value = true;
  const submitted = JSON.parse(JSON.stringify(values.value));
  try {
    // The secret goes first: if it fails there is no point writing a client id that cannot be
    // used, and the message says which half went wrong.
    if (secret.value) {
      await apiPost('/api/igdb/secret', { secret: secret.value });
      secret.value = '';
    }
    const result = await apiPatch<{ status?: boolean }>('/api/config', submitted);
    if (result.status === false) throw new Error('save-rejected');
    original.value = submitted;
    note(t('ui.metadata.saved'));
    status.value = await apiGet<IgdbStatus>('/api/igdb/status');
  } catch {
    note(t('ui.settings.errors.save'), 'warning');
  } finally {
    saving.value = false;
  }
}

async function clearSecret() {
  busy.value = 'secret';
  try {
    await apiPost('/api/igdb/secret', { secret: '' });
    secret.value = '';
    status.value = await apiGet<IgdbStatus>('/api/igdb/status');
    note(t('ui.metadata.secretCleared'));
  } catch {
    note(t('ui.metadata.secretFailed'), 'warning');
  } finally {
    busy.value = '';
  }
}

async function verify() {
  busy.value = 'verify';
  try {
    const result = await apiPost<{ status?: boolean; error?: string }>('/api/igdb/verify');
    status.value = await apiGet<IgdbStatus>('/api/igdb/status');
    note(
      result.status ? t('ui.metadata.verifyOk') : result.error || t('ui.metadata.verifyFailed'),
      result.status ? 'success' : 'warning',
    );
  } catch {
    note(t('ui.metadata.verifyFailed'), 'warning');
  } finally {
    busy.value = '';
  }
}

let pollTimer: ReturnType<typeof setTimeout> | undefined;

// The host queues the pass instead of running it inside the request, so progress is read back
// from the status endpoint rather than returned. Polling stops as soon as the pass is done,
// and on unmount, so a page left open does not keep asking forever.
function pollUntilIdle() {
  clearTimeout(pollTimer);
  pollTimer = setTimeout(async () => {
    try {
      const next = await apiGet<IgdbStatus>('/api/igdb/status');
      status.value = next;
      if (next.resolving) {
        pollUntilIdle();
      } else {
        note(
          next.last_error || t('ui.metadata.resolveDone'),
          next.last_error ? 'warning' : 'success',
        );
      }
    } catch {
      /* A failed poll is not worth reporting; the next action will show the real state. */
    }
  }, 4000);
}

async function resolveLibrary(force: boolean) {
  busy.value = 'resolve';
  try {
    await apiPost('/api/igdb/resolve', { force });
    status.value = await apiGet<IgdbStatus>('/api/igdb/status');
    note(t('ui.metadata.resolveStarted'));
    pollUntilIdle();
  } catch {
    note(t('ui.metadata.resolveFailed'), 'warning');
  } finally {
    busy.value = '';
  }
}

async function clearCache() {
  busy.value = 'cache';
  try {
    const result = await apiDelete<{ removed?: number }>('/api/igdb/cache');
    note(t('ui.metadata.cacheCleared', { count: result.removed ?? 0 }));
  } catch {
    note(t('ui.metadata.cacheFailed'), 'warning');
  } finally {
    busy.value = '';
  }
}

onMounted(() => {
  void load();
});

onUnmounted(() => {
  clearTimeout(pollTimer);
});
</script>

<template>
  <!-- Only the form lives here. The surrounding row, its icon and its heading belong to
       IntegrationsView, whose styles for them are scoped to that file. -->
  <div class="integration-settings metadata-settings">
    <div class="metadata-settings__status">
      <StatusBadge :label="statusLabel" :tone="statusTone" compact />
      <span v-if="status?.resolving" class="metadata-settings__note">
        {{ t('ui.metadata.resolving') }}
      </span>
    </div>

    <InlineAlert v-if="failed" tone="warning" :title="t('ui.settings.errors.load')" />
    <InlineAlert v-else-if="message" :tone="messageTone" :title="message" />
    <InlineAlert
      v-else-if="status?.last_error"
      tone="warning"
      :title="t('ui.metadata.status.failing')"
    >
      {{ status.last_error }}
    </InlineAlert>

    <form @submit.prevent="save">
      <fieldset :disabled="loading || saving">
        <div class="vs-settings-group">
          <SettingRow :label="t('ui.metadata.fields.enabled')" control-id="igdb_enabled">
            <label class="vs-switch">
              <input id="igdb_enabled" v-model="values.igdb_enabled" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
            </label>
          </SettingRow>

          <SettingRow
            :label="t('ui.metadata.fields.clientId')"
            :description="t('ui.metadata.fields.clientIdHint')"
            control-id="igdb_client_id"
          >
            <input
              id="igdb_client_id"
              v-model="values.igdb_client_id"
              class="vs-input"
              type="text"
              autocomplete="off"
              spellcheck="false"
            />
          </SettingRow>

          <SettingRow :label="t('ui.metadata.fields.secret')" control-id="igdb_secret">
            <template #description>
              {{
                status?.configured
                  ? t('ui.metadata.fields.secretStored')
                  : t('ui.metadata.fields.secretHint')
              }}
              <!-- The path is shown because "it is not picking up my secret" is otherwise a
                   guess about a location nobody can see from here. -->
              <code v-if="status?.secret_file" class="metadata-settings__path">{{
                status.secret_file
              }}</code>
            </template>
            <div class="metadata-settings__secret">
              <input
                id="igdb_secret"
                v-model="secret"
                class="vs-input"
                type="password"
                autocomplete="new-password"
                spellcheck="false"
                :placeholder="
                  status?.configured
                    ? t('ui.metadata.fields.secretReplace')
                    : t('ui.metadata.fields.secretPlaceholder')
                "
              />
              <AppButton
                v-if="status?.configured"
                :label="t('_common.remove')"
                variant="tertiary"
                :disabled="busy === 'secret'"
                @click="clearSecret"
              />
            </div>
          </SettingRow>

          <SettingRow
            v-if="enabled"
            :label="t('ui.metadata.fields.autoResolve')"
            :description="t('ui.metadata.fields.autoResolveHint')"
            control-id="igdb_auto_resolve"
          >
            <label class="vs-switch">
              <input id="igdb_auto_resolve" v-model="values.igdb_auto_resolve" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
            </label>
          </SettingRow>

          <SettingRow
            v-if="enabled"
            :label="t('ui.metadata.fields.nameMatch')"
            :description="t('ui.metadata.fields.nameMatchHint')"
            control-id="igdb_allow_name_match"
          >
            <label class="vs-switch">
              <input
                id="igdb_allow_name_match"
                v-model="values.igdb_allow_name_match"
                type="checkbox"
              />
              <span class="vs-switch__track" aria-hidden="true" />
            </label>
          </SettingRow>

          <SettingRow
            v-if="isWindows"
            :label="t('ui.metadata.fields.playniteSync')"
            :description="t('ui.metadata.fields.playniteSyncHint')"
            control-id="playnite_sync_metadata"
          >
            <label class="vs-switch">
              <input
                id="playnite_sync_metadata"
                v-model="values.playnite_sync_metadata"
                type="checkbox"
              />
              <span class="vs-switch__track" aria-hidden="true" />
            </label>
          </SettingRow>

          <SettingRow
            v-if="enabled"
            :label="t('ui.metadata.fields.cacheTtl')"
            :description="t('ui.metadata.fields.cacheTtlHint')"
            control-id="igdb_cache_ttl_days"
          >
            <input
              id="igdb_cache_ttl_days"
              v-model.number="values.igdb_cache_ttl_days"
              class="vs-input"
              type="number"
              min="0"
              step="1"
            />
          </SettingRow>
        </div>
      </fieldset>

      <div class="metadata-settings__actions">
        <AppButton
          :label="t(saving ? 'ui.settings.saving' : 'ui.settings.save')"
          variant="primary"
          type="submit"
          :disabled="loading || saving || !dirty"
        />
        <AppButton
          :label="t('ui.settings.discard')"
          variant="secondary"
          :disabled="!dirty || saving"
          @click="values = JSON.parse(JSON.stringify(original))"
        />
        <AppButton
          :label="t('ui.metadata.actions.verify')"
          variant="secondary"
          :disabled="!ready || busy === 'verify'"
          @click="verify"
        />
        <AppButton
          :label="t('ui.metadata.actions.resolve')"
          variant="secondary"
          :disabled="!ready || !enabled || busy === 'resolve'"
          @click="resolveLibrary(false)"
        />
        <AppButton
          :label="t('ui.metadata.actions.refresh')"
          variant="tertiary"
          :disabled="!ready || !enabled || busy === 'resolve'"
          @click="resolveLibrary(true)"
        />
        <AppButton
          :label="t('ui.metadata.actions.clearCache')"
          variant="tertiary"
          :disabled="busy === 'cache'"
          @click="clearCache"
        />
      </div>
    </form>
  </div>
</template>

<style scoped>
.metadata-settings {
  display: grid;
  gap: var(--vs-space-12);
  padding-top: var(--vs-space-12);
}

.metadata-settings__status {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.metadata-settings__note {
  font-size: 0.875rem;
  color: var(--vs-color-text-muted);
}

.metadata-settings fieldset {
  border: 0;
  padding: 0;
  margin: 0;
  min-width: 0;
}

.metadata-settings__secret {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.metadata-settings__path {
  display: block;
  margin-top: var(--vs-space-4, 4px);
  font-family: var(--vs-font-mono, ui-monospace, monospace);
  font-size: 0.8125rem;
  overflow-wrap: anywhere;
  color: var(--vs-color-text-muted);
}

.metadata-settings__secret input {
  flex: 1;
  min-width: 12rem;
}

.metadata-settings__actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  padding-top: var(--vs-space-12);
}
</style>
