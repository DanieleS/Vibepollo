<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiDelete, apiGet, apiPost } from '@/api/client';
import { AppButton, InlineAlert, StatusBadge } from '@/components/ui';
import type { StatusTone } from '@/components/ui/types';

// Mirrors platf::session_unlock::status(). The password is never part of it.
interface UnlockEvent {
  at?: number;
  client?: string;
  outcome?: string;
  detail?: string;
}

interface UnlockStatus {
  enabled?: boolean;
  account?: string;
  console_account?: string;
  provider_installed?: boolean;
  hello_only?: boolean;
  relock?: boolean;
  locked?: boolean;
  events?: UnlockEvent[];
}

interface UnlockMutation extends UnlockStatus {
  status?: boolean;
  error?: string;
}

const UNLOCK_PERM = 0x00400000;

const { t } = useI18n();

const state = ref<UnlockStatus | null>(null);
const allowedDevices = ref<string[]>([]);
const loading = ref(true);
const loadError = ref('');
const password = ref('');
const saving = ref(false);
const busy = ref(false);
const fieldError = ref('');
const passwordInput = ref<HTMLInputElement | null>(null);

const enabled = computed(() => Boolean(state.value?.enabled));
const helloOnly = computed(() => Boolean(state.value?.hello_only));
const relock = computed(() => Boolean(state.value?.relock));
const account = computed(() => state.value?.console_account || state.value?.account || '');

const lastEvent = computed<UnlockEvent | null>(() => {
  const events = state.value?.events;
  return events && events.length ? events[events.length - 1] : null;
});

const statusState = computed<{ label: string; tone: StatusTone }>(() => {
  if (helloOnly.value) {
    return { label: t('ui.settings.unlock.status_blocked'), tone: 'warning' };
  }
  if (enabled.value) {
    if (lastEvent.value?.outcome === 'rejected') {
      return { label: t('ui.settings.unlock.status_rejected'), tone: 'danger' };
    }
    return { label: t('ui.settings.unlock.status_ready'), tone: 'success' };
  }
  return { label: t('ui.settings.unlock.status_off'), tone: 'neutral' };
});

function outcomeLabel(outcome?: string): string {
  switch (outcome) {
    case 'unlocked':
      return t('ui.settings.unlock.outcome_unlocked');
    case 'relocked':
      return t('ui.settings.unlock.outcome_relocked');
    case 'rejected':
      return t('ui.settings.unlock.outcome_rejected');
    case 'hello_only':
      return t('ui.settings.unlock.outcome_hello_only');
    case 'unavailable':
      return t('ui.settings.unlock.outcome_unavailable');
    default:
      return t('ui.settings.unlock.outcome_failed');
  }
}

const lastUnlockSummary = computed(() => {
  const event = lastEvent.value;
  if (!event) return t('ui.settings.unlock.last_unlock_none');
  const who = event.client ? `${event.client} — ` : '';
  return `${who}${outcomeLabel(event.outcome)}`;
});

async function loadStatus(): Promise<void> {
  try {
    state.value = await apiGet<UnlockStatus>('/api/unlock');
  } catch {
    state.value = null;
    loadError.value = t('ui.settings.unlock.load_error');
  }
}

async function loadAllowedDevices(): Promise<void> {
  try {
    const response = await apiGet<{ named_certs?: Array<{ name?: string; perm?: number | string }> }>(
      '/api/clients/list',
    );
    const certs = Array.isArray(response?.named_certs) ? response.named_certs : [];
    allowedDevices.value = certs
      .filter((cert) => (Number(cert?.perm ?? 0) & UNLOCK_PERM) !== 0)
      .map((cert) => String(cert?.name ?? '').trim())
      .filter(Boolean);
  } catch {
    allowedDevices.value = [];
  }
}

function applyMutation(result: UnlockMutation): boolean {
  if (result.status === false) {
    fieldError.value = result.error || t('ui.settings.unlock.load_error');
    return false;
  }
  // A success returns the fresh status().
  state.value = result;
  return true;
}

async function savePassword(): Promise<void> {
  if (saving.value || !password.value) return;
  saving.value = true;
  fieldError.value = '';
  try {
    const result = await apiPost<UnlockMutation>('/api/unlock', { password: password.value });
    if (applyMutation(result)) {
      password.value = '';
    }
  } catch {
    fieldError.value = t('ui.settings.unlock.load_error');
  } finally {
    saving.value = false;
  }
}

async function disableFeature(): Promise<void> {
  if (busy.value) return;
  busy.value = true;
  fieldError.value = '';
  try {
    state.value = await apiDelete<UnlockStatus>('/api/unlock');
  } catch {
    fieldError.value = t('ui.settings.unlock.load_error');
  } finally {
    busy.value = false;
  }
}

function onToggleEnabled(event: Event): void {
  const checked = (event.target as HTMLInputElement).checked;
  if (!checked) {
    void disableFeature();
  } else {
    // Enabling needs the password: keep the switch off until the save lands, and focus the field.
    (event.target as HTMLInputElement).checked = false;
    passwordInput.value?.focus();
  }
}

async function onToggleRelock(event: Event): Promise<void> {
  if (busy.value) return;
  const checked = (event.target as HTMLInputElement).checked;
  busy.value = true;
  try {
    const result = await apiPost<UnlockMutation>('/api/unlock/settings', { relock: checked });
    applyMutation(result);
  } catch {
    fieldError.value = t('ui.settings.unlock.load_error');
  } finally {
    busy.value = false;
  }
}

onMounted(async () => {
  loading.value = true;
  await Promise.all([loadStatus(), loadAllowedDevices()]);
  loading.value = false;
});
</script>

<template>
  <div class="unlock-settings">
    <InlineAlert
      v-if="helloOnly"
      class="unlock-settings__alert"
      tone="warning"
      announce="polite"
      :title="t('ui.settings.unlock.status_blocked')"
    >
      {{ t('ui.settings.unlock.hello_only_warning') }}
    </InlineAlert>

    <article class="unlock-item">
      <div class="unlock-item__copy">
        <div class="unlock-item__title">
          <strong>{{ t('ui.settings.unlock.enable_label') }}</strong>
          <StatusBadge :label="statusState.label" :tone="statusState.tone" compact />
        </div>
        <p>{{ t('ui.settings.unlock.enable_detail') }}</p>
      </div>
      <label class="vs-switch">
        <input
          type="checkbox"
          :checked="enabled"
          :disabled="loading || busy"
          @change="onToggleEnabled"
        />
        <span class="vs-switch__track" aria-hidden="true" />
        <span class="visually-hidden">{{ t('ui.settings.unlock.enable_label') }}</span>
      </label>
    </article>

    <article class="unlock-item">
      <div class="unlock-item__copy">
        <div class="unlock-item__title">
          <strong>{{ t('ui.settings.unlock.account_label') }}</strong>
        </div>
        <p class="unlock-account">
          {{ account || t('ui.settings.unlock.account_unknown') }}
        </p>
      </div>
    </article>

    <article v-if="!enabled" class="unlock-item unlock-item--password">
      <div class="unlock-item__copy">
        <div class="unlock-item__title">
          <strong>{{ t('ui.settings.unlock.password_label') }}</strong>
        </div>
      </div>
      <div class="unlock-password">
        <input
          ref="passwordInput"
          class="vs-input"
          type="password"
          autocomplete="off"
          :placeholder="t('ui.settings.unlock.password_placeholder')"
          :disabled="saving || helloOnly"
          v-model="password"
          @keydown.enter.prevent="savePassword"
        />
        <AppButton
          :label="t('ui.settings.unlock.check_and_save')"
          :busy="saving"
          :busy-label="t('ui.settings.unlock.saving')"
          :disabled="!password || helloOnly"
          variant="secondary"
          @click="savePassword"
        />
      </div>
    </article>

    <article class="unlock-item">
      <div class="unlock-item__copy">
        <div class="unlock-item__title">
          <strong>{{ t('ui.settings.unlock.relock_label') }}</strong>
        </div>
        <p>{{ t('ui.settings.unlock.relock_detail') }}</p>
      </div>
      <label class="vs-switch">
        <input
          type="checkbox"
          :checked="relock"
          :disabled="loading || busy || !enabled"
          @change="onToggleRelock"
        />
        <span class="vs-switch__track" aria-hidden="true" />
        <span class="visually-hidden">{{ t('ui.settings.unlock.relock_label') }}</span>
      </label>
    </article>

    <div class="unlock-summary">
      <div class="unlock-summary__row">
        <span class="unlock-summary__label">{{ t('ui.settings.unlock.allowed_label') }}</span>
        <span class="unlock-summary__value">
          <template v-if="allowedDevices.length">{{ allowedDevices.join(', ') }}</template>
          <template v-else>{{ t('ui.settings.unlock.allowed_none') }}</template>
          <RouterLink class="unlock-summary__link" :to="{ name: 'devices' }">
            {{ t('ui.settings.unlock.manage_devices') }}
          </RouterLink>
        </span>
      </div>
      <div class="unlock-summary__row">
        <span class="unlock-summary__label">{{ t('ui.settings.unlock.last_unlock_label') }}</span>
        <span class="unlock-summary__value">{{ lastUnlockSummary }}</span>
      </div>
    </div>

    <p v-if="fieldError" class="unlock-message unlock-message--error" role="alert">
      {{ fieldError }}
    </p>
    <p v-else-if="loadError" class="unlock-message unlock-message--error" role="alert">
      {{ loadError }}
    </p>
  </div>
</template>

<style scoped>
.unlock-settings {
  display: grid;
  width: 100%;
  gap: 0;
}

.unlock-settings__alert {
  margin: var(--vs-space-16) var(--vs-space-20);
}

.unlock-item {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16) var(--vs-space-20);
  background: transparent;
}

.unlock-item + .unlock-item {
  border-top: 1px solid var(--vs-color-border-subtle);
}

.unlock-item--password {
  align-items: start;
}

.unlock-item__copy {
  display: grid;
  min-width: 0;
  gap: var(--vs-space-8);
}

.unlock-item__title {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.unlock-item p,
.unlock-message {
  margin: 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-body);
}

.unlock-account {
  font-family: var(--vs-type-family-mono, monospace);
  color: var(--vs-color-text-primary);
}

.unlock-password {
  display: grid;
  grid-template-columns: minmax(200px, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-8);
}

.unlock-summary {
  display: grid;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16) var(--vs-space-20);
  border-top: 1px solid var(--vs-color-border-subtle);
}

.unlock-summary__row {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  font-size: var(--vs-type-size-helper);
}

.unlock-summary__label {
  min-width: 96px;
  color: var(--vs-color-text-muted);
}

.unlock-summary__value {
  color: var(--vs-color-text-secondary);
}

.unlock-summary__link {
  margin-left: var(--vs-space-8);
  color: var(--vs-color-accent-default);
}

.unlock-message--error {
  padding: 0 var(--vs-space-20) var(--vs-space-16);
  color: var(--vs-color-status-danger);
}

@media (max-width: 899px) {
  .unlock-item {
    grid-template-columns: minmax(0, 1fr);
  }

  .unlock-password {
    grid-template-columns: minmax(0, 1fr);
  }
}
</style>
