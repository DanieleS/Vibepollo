<script setup lang="ts">
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const config = store.config;
</script>

<template>
  <div id="telemetry" class="config-page space-y-5">
    <div
      class="rounded-xl border border-dark/10 bg-light/60 px-4 py-3 dark:border-light/10 dark:bg-dark/40 sm:px-5 sm:py-4"
    >
      <div class="flex items-start gap-3">
        <span
          class="mt-0.5 inline-flex h-8 w-8 shrink-0 items-center justify-center rounded-lg bg-primary/10 text-primary"
        >
          <i class="fas fa-heart-pulse text-sm" />
        </span>
        <div class="min-w-0">
          <h3 class="text-sm font-semibold leading-tight">{{ $t('telemetry.config_header') }}</h3>
          <p class="mt-1 text-xs leading-relaxed opacity-70">
            {{ $t('telemetry.config_desc') }}
          </p>
        </div>
      </div>
    </div>

    <ConfigFieldRenderer v-model="config.scry_enabled" setting-key="scry_enabled" class="mb-3" />

    <ConfigFieldRenderer
      v-model="config.scry_profiles_dir"
      setting-key="scry_profiles_dir"
      class="mb-3"
    />

    <ConfigFieldRenderer v-model="config.scry_tick_ms" setting-key="scry_tick_ms" class="mb-3" />
  </div>
</template>

<style scoped></style>
