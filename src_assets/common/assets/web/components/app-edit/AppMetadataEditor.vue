<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { apiGet, apiPost } from '@/api/client';
import { AppButton, InlineAlert, StatusBadge } from '@/components/ui';

interface StoreId {
  store: string;
  id: string;
}

interface AppMetadata {
  present?: boolean;
  description?: string;
  genres?: string[];
  developers?: string[];
  publishers?: string[];
  release_date?: string;
  community_score?: number;
  critic_score?: number;
  last_played?: string;
  playtime_minutes?: number;
  has_background?: boolean;
  source?: string;
  igdb_id?: string;
  locked?: boolean;
  store_ids?: StoreId[];
}

interface Candidate {
  igdb_id: string;
  name: string;
  summary?: string;
  release_date?: string;
  cover_url?: string;
}

const props = defineProps<{ uuid: string; name: string }>();

const { t } = useI18n();

const metadata = ref<AppMetadata | null>(null);
const loading = ref(false);
const busy = ref('');
const error = ref('');
const notice = ref('');

// Edited copy of the free-text fields. Lists are edited as comma-separated text because a
// genre list is three words, and a chip editor would be more chrome than content.
const draft = ref({
  description: '',
  genres: '',
  developers: '',
  publishers: '',
  release_date: '',
});

const pickerOpen = ref(false);
const query = ref('');
const candidates = ref<Candidate[]>([]);
const searching = ref(false);

const linked = computed(() => Boolean(metadata.value?.igdb_id));
const sourceLabel = computed(() => {
  const source = metadata.value?.source;
  if (!source) return t('ui.appMetadata.source.none');
  if (source === 'igdb') return t('ui.appMetadata.source.igdb');
  if (source === 'playnite') return t('ui.appMetadata.source.playnite');
  if (source === 'manual') return t('ui.appMetadata.source.manual');
  return source;
});
const sourceTone = computed(() => {
  const source = metadata.value?.source;
  if (!source) return 'neutral' as const;
  return source === 'manual' ? ('info' as const) : ('success' as const);
});

const storeIds = computed(() => metadata.value?.store_ids ?? []);

const dirty = computed(() => {
  const current = metadata.value;
  if (!current) return false;
  return (
    draft.value.description !== (current.description ?? '') ||
    draft.value.genres !== (current.genres ?? []).join(', ') ||
    draft.value.developers !== (current.developers ?? []).join(', ') ||
    draft.value.publishers !== (current.publishers ?? []).join(', ') ||
    draft.value.release_date !== (current.release_date ?? '')
  );
});

function adopt(next: AppMetadata) {
  metadata.value = next;
  draft.value = {
    description: next.description ?? '',
    genres: (next.genres ?? []).join(', '),
    developers: (next.developers ?? []).join(', '),
    publishers: (next.publishers ?? []).join(', '),
    release_date: next.release_date ?? '',
  };
}

function splitList(value: string): string[] {
  return value
    .split(',')
    .map((entry) => entry.trim())
    .filter(Boolean);
}

async function load() {
  if (!props.uuid) return;
  loading.value = true;
  error.value = '';
  try {
    adopt(await apiGet<AppMetadata>(`/api/apps/${encodeURIComponent(props.uuid)}/metadata`));
  } catch {
    error.value = t('ui.appMetadata.errors.load');
  } finally {
    loading.value = false;
  }
}

async function saveManual() {
  busy.value = 'save';
  error.value = '';
  notice.value = '';
  try {
    adopt(
      await apiPost<AppMetadata>(`/api/apps/${encodeURIComponent(props.uuid)}/metadata`, {
        description: draft.value.description,
        genres: splitList(draft.value.genres),
        developers: splitList(draft.value.developers),
        publishers: splitList(draft.value.publishers),
        release_date: draft.value.release_date,
        locked: true,
      }),
    );
    notice.value = t('ui.appMetadata.saved');
  } catch {
    error.value = t('ui.appMetadata.errors.save');
  } finally {
    busy.value = '';
  }
}

async function unlock() {
  busy.value = 'save';
  error.value = '';
  notice.value = '';
  try {
    adopt(
      await apiPost<AppMetadata>(`/api/apps/${encodeURIComponent(props.uuid)}/metadata`, {
        locked: false,
      }),
    );
    notice.value = t('ui.appMetadata.unlocked');
  } catch {
    error.value = t('ui.appMetadata.errors.save');
  } finally {
    busy.value = '';
  }
}

async function resolveNow(force: boolean) {
  busy.value = 'resolve';
  error.value = '';
  notice.value = '';
  try {
    const result = await apiPost<{
      status?: boolean;
      error?: string;
      igdb_id?: string;
      metadata?: AppMetadata;
    }>('/api/igdb/resolve', { uuid: props.uuid, force });
    if (result.metadata) adopt(result.metadata);
    if (result.error) error.value = result.error;
    else if (!result.igdb_id) notice.value = t('ui.appMetadata.noMatch');
    else notice.value = t('ui.appMetadata.matched');
  } catch {
    error.value = t('ui.appMetadata.errors.resolve');
  } finally {
    busy.value = '';
  }
}

async function search() {
  const term = query.value.trim();
  if (!term) return;
  searching.value = true;
  error.value = '';
  try {
    const response = await apiGet<{ results?: Candidate[]; error?: string }>(
      `/api/igdb/search?q=${encodeURIComponent(term)}&limit=20`,
    );
    candidates.value = response.results ?? [];
    // A host with no credentials answers with an error rather than an empty list, and saying
    // "no results" there would send the user looking for a game that was never searched for.
    if (response.error) error.value = response.error;
    else if (!candidates.value.length) notice.value = t('ui.appMetadata.noResults');
  } catch {
    error.value = t('ui.appMetadata.errors.search');
  } finally {
    searching.value = false;
  }
}

async function pick(candidate: Candidate) {
  busy.value = 'pick';
  error.value = '';
  notice.value = '';
  try {
    adopt(
      await apiPost<AppMetadata>(`/api/apps/${encodeURIComponent(props.uuid)}/metadata`, {
        igdb_id: candidate.igdb_id,
      }),
    );
    pickerOpen.value = false;
    candidates.value = [];
    notice.value = t('ui.appMetadata.linked', { name: candidate.name });
  } catch {
    error.value = t('ui.appMetadata.errors.link');
  } finally {
    busy.value = '';
  }
}

async function unlink() {
  busy.value = 'pick';
  error.value = '';
  notice.value = '';
  try {
    adopt(
      await apiPost<AppMetadata>(`/api/apps/${encodeURIComponent(props.uuid)}/metadata`, {
        igdb_id: '',
      }),
    );
    notice.value = t('ui.appMetadata.unlinked');
  } catch {
    error.value = t('ui.appMetadata.errors.link');
  } finally {
    busy.value = '';
  }
}

function openPicker() {
  pickerOpen.value = true;
  if (!query.value) query.value = props.name;
}

watch(() => props.uuid, load, { immediate: true });
</script>

<template>
  <section class="editor-section" aria-labelledby="metadata-heading">
    <div class="editor-section__heading">
      <h2 id="metadata-heading">{{ t('ui.appMetadata.title') }}</h2>
      <p>{{ t('ui.appMetadata.description') }}</p>
    </div>

    <div class="editor-group">
      <div class="metadata-status">
        <StatusBadge :label="sourceLabel" :tone="sourceTone" compact />
        <span v-if="metadata?.locked" class="metadata-chip">{{ t('ui.appMetadata.locked') }}</span>
        <span v-if="linked" class="metadata-chip">IGDB #{{ metadata?.igdb_id }}</span>
      </div>

      <InlineAlert v-if="error" tone="warning" :title="error" />
      <InlineAlert v-else-if="notice" tone="success" :title="notice" />

      <p v-if="storeIds.length" class="metadata-hint">
        {{ t('ui.appMetadata.matchedOn') }}
        <span v-for="entry in storeIds" :key="entry.store + entry.id" class="metadata-chip">
          {{ entry.store }}:{{ entry.id }}
        </span>
      </p>
      <p v-else class="metadata-hint">{{ t('ui.appMetadata.noStoreId') }}</p>

      <div class="editor-grid">
        <label class="vs-field editor-field editor-field--wide">
          <span class="vs-field__label">{{ t('ui.appMetadata.fields.description') }}</span>
          <textarea v-model="draft.description" class="vs-input" rows="4" />
        </label>
        <label class="vs-field editor-field">
          <span class="vs-field__label">{{ t('ui.appMetadata.fields.genres') }}</span>
          <input v-model="draft.genres" class="vs-input" type="text" />
        </label>
        <label class="vs-field editor-field">
          <span class="vs-field__label">{{ t('ui.appMetadata.fields.releaseDate') }}</span>
          <input v-model="draft.release_date" class="vs-input" type="text" placeholder="YYYY-MM-DD" />
        </label>
        <label class="vs-field editor-field">
          <span class="vs-field__label">{{ t('ui.appMetadata.fields.developers') }}</span>
          <input v-model="draft.developers" class="vs-input" type="text" />
        </label>
        <label class="vs-field editor-field">
          <span class="vs-field__label">{{ t('ui.appMetadata.fields.publishers') }}</span>
          <input v-model="draft.publishers" class="vs-input" type="text" />
        </label>
      </div>

      <div class="metadata-actions">
        <AppButton
          :label="t('ui.appMetadata.actions.save')"
          variant="primary"
          :disabled="!dirty || busy === 'save'"
          @click="saveManual"
        />
        <AppButton
          :label="t('ui.appMetadata.actions.lookup')"
          variant="secondary"
          :disabled="busy === 'resolve'"
          @click="resolveNow(true)"
        />
        <AppButton
          :label="t('ui.appMetadata.actions.choose')"
          variant="secondary"
          @click="openPicker"
        />
        <AppButton
          v-if="linked"
          :label="t('ui.appMetadata.actions.unlink')"
          variant="tertiary"
          :disabled="busy === 'pick'"
          @click="unlink"
        />
        <AppButton
          v-if="metadata?.locked"
          :label="t('ui.appMetadata.actions.unlock')"
          variant="tertiary"
          :disabled="busy === 'save'"
          @click="unlock"
        />
        <AppButton
          :label="t('ui.appMetadata.actions.reload')"
          variant="tertiary"
          :disabled="loading"
          @click="load"
        />
      </div>

      <div v-if="pickerOpen" class="metadata-picker">
        <div class="metadata-picker__search">
          <input
            v-model="query"
            class="vs-input"
            type="search"
            :placeholder="t('ui.appMetadata.searchPlaceholder')"
            @keydown.enter.prevent="search"
          />
          <AppButton
            :label="t('_common.search')"
            variant="secondary"
            :busy="searching"
            @click="search"
          />
          <AppButton
            :label="t('_common.cancel')"
            variant="tertiary"
            @click="pickerOpen = false"
          />
        </div>
        <ul v-if="candidates.length" class="metadata-candidates">
          <li v-for="candidate in candidates" :key="candidate.igdb_id">
            <button type="button" class="metadata-candidate" @click="pick(candidate)">
              <img
                v-if="candidate.cover_url"
                :src="candidate.cover_url"
                alt=""
                width="46"
                height="62"
                loading="lazy"
              />
              <span class="metadata-candidate__text">
                <strong>{{ candidate.name }}</strong>
                <small v-if="candidate.release_date">{{ candidate.release_date }}</small>
                <small v-if="candidate.summary" class="metadata-candidate__summary">
                  {{ candidate.summary }}
                </small>
              </span>
            </button>
          </li>
        </ul>
      </div>
    </div>
  </section>
</template>

<style scoped>
.metadata-status {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.metadata-chip {
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-pill, 999px);
  padding: 0 var(--vs-space-8);
  font-size: 0.8125rem;
  color: var(--vs-color-text-muted);
}

.metadata-hint {
  margin: 0;
  font-size: 0.875rem;
  color: var(--vs-color-text-muted);
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.metadata-actions {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.metadata-picker {
  display: grid;
  gap: var(--vs-space-12);
  border-top: var(--vs-border-width) solid var(--vs-color-border-subtle);
  padding-top: var(--vs-space-12);
}

.metadata-picker__search {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.metadata-picker__search input {
  flex: 1;
  min-width: 14rem;
}

.metadata-candidates {
  list-style: none;
  margin: 0;
  padding: 0;
  display: grid;
  gap: var(--vs-space-8);
  max-height: 22rem;
  overflow-y: auto;
}

.metadata-candidate {
  display: flex;
  gap: var(--vs-space-12);
  align-items: flex-start;
  width: 100%;
  text-align: left;
  background: transparent;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  padding: var(--vs-space-8);
  cursor: pointer;
  color: inherit;
}

.metadata-candidate:hover {
  border-color: var(--vs-color-border-strong, var(--vs-color-border-subtle));
}

.metadata-candidate__text {
  display: grid;
  gap: 2px;
  min-width: 0;
}

.metadata-candidate__summary {
  display: -webkit-box;
  -webkit-line-clamp: 2;
  line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
  color: var(--vs-color-text-muted);
}
</style>
