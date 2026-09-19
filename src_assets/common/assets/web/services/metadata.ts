import { apiGet, apiPost } from '@/api/client';

export type MetadataResultStatus =
  | 'matched'
  | 'matched_ambiguous'
  | 'unmatched'
  | 'skipped'
  | 'failed';

export interface MetadataBulkResult {
  uuid: string;
  name: string;
  status: MetadataResultStatus;
  igdb_id?: string;
  igdb_name?: string;
  reason?: string;
  background?: boolean;
  cover?: boolean;
}

export interface MetadataBulkOptions {
  refresh_existing: boolean;
  download_background: boolean;
  download_cover: boolean;
  uuids: string[];
}

export interface MetadataBulkStatus {
  running: boolean;
  cancelled: boolean;
  total: number;
  processed: number;
  matched: number;
  unmatched: number;
  skipped: number;
  failed: number;
  current: string;
  error: string;
  started_at: string;
  finished_at: string;
  options: MetadataBulkOptions;
  results: MetadataBulkResult[];
}

interface MetadataBulkMutation extends Record<string, unknown> {
  status?: unknown;
  error?: unknown;
  job?: MetadataBulkStatus;
}

export async function fetchMetadataBulkStatus(): Promise<MetadataBulkStatus> {
  return apiGet<MetadataBulkStatus>('/api/metadata/igdb/bulk');
}

export async function startMetadataBulk(
  options: Partial<MetadataBulkOptions>,
): Promise<MetadataBulkStatus> {
  const response = await apiPost<MetadataBulkMutation>('/api/metadata/igdb/bulk', {
    refresh_existing: options.refresh_existing ?? false,
    download_background: options.download_background ?? true,
    download_cover: options.download_cover ?? true,
    uuids: options.uuids ?? [],
  });
  if (response.status !== true) {
    throw new Error(
      typeof response.error === 'string' && response.error
        ? response.error
        : 'metadata-bulk-start-failed',
    );
  }
  if (!response.job) throw new Error('metadata-bulk-start-failed');
  return response.job;
}

export async function cancelMetadataBulk(): Promise<MetadataBulkStatus | null> {
  const response = await apiPost<MetadataBulkMutation>('/api/metadata/igdb/bulk/cancel', {});
  return response.job ?? null;
}
