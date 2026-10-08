<script setup lang="ts">
// The Playnite extensions whose data Vibepollo reads for clients, and whether it found any.
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';
import { StatusBadge, type StatusTone } from '@/components/ui';

export interface PlayniteExtensionStatus {
  enabled?: boolean;
  known?: boolean;
  found?: boolean;
  data_dir?: string;
  url?: string;
}

const props = defineProps<{
  extensions?: Record<string, PlayniteExtensionStatus>;
}>();

const { t } = useI18n();

const catalogue = [
  {
    id: 'successstory',
    name: 'SuccessStory',
    url: 'https://github.com/Lacro59/playnite-successstory-plugin',
  },
  {
    id: 'gameactivity',
    name: 'GameActivity',
    url: 'https://github.com/Lacro59/playnite-gameactivity-plugin',
  },
];

const rows = computed(() =>
  catalogue.map((extension) => {
    const status = props.extensions?.[extension.id];
    let label = t('playnite.extension_unknown');
    let tone: StatusTone = 'neutral';
    if (status?.enabled === false) {
      label = t('playnite.extension_disabled');
    } else if (status?.found) {
      label = t('playnite.extension_found');
      tone = 'success';
    } else if (status?.known) {
      label = t('playnite.extension_not_found');
      tone = 'warning';
    }
    return {
      ...extension,
      url: status?.url || extension.url,
      description: t(`playnite.${extension.id}_desc`),
      label,
      tone,
      title: status?.data_dir || undefined,
    };
  }),
);
</script>

<template>
  <section class="playnite-extensions" aria-labelledby="playnite-extensions-heading">
    <h3 id="playnite-extensions-heading">{{ t('playnite.extensions_title') }}</h3>
    <small>{{ t('playnite.extensions_desc') }}</small>
    <ul>
      <li v-for="row in rows" :key="row.id" :title="row.title">
        <span class="playnite-extensions__name">{{ row.name }}</span>
        <span class="playnite-extensions__desc">{{ row.description }}</span>
        <StatusBadge :label="row.label" :tone="row.tone" compact />
        <a :href="row.url" target="_blank" rel="noopener noreferrer">{{
          t('playnite.extension_link')
        }}</a>
      </li>
    </ul>
  </section>
</template>

<style scoped>
.playnite-extensions {
  display: grid;
  gap: var(--vs-space-8);
  padding-top: var(--vs-space-16);
  max-width: 52rem;
}

.playnite-extensions small {
  color: var(--vs-color-text-secondary);
}

.playnite-extensions ul {
  display: grid;
  gap: var(--vs-space-8);
  padding: 0;
  margin: 0;
  list-style: none;
}

.playnite-extensions li {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8) var(--vs-space-12);
}

.playnite-extensions__name {
  font-weight: 600;
}

.playnite-extensions__desc {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}
</style>
