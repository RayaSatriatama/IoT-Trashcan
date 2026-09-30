export interface PaginationResult<T> {
  items: T[];
  page: number;
  totalPages: number;
}

export function pageCount(itemCount: number, pageSize: number): number {
  if (pageSize <= 0) return 1;
  return Math.max(1, Math.ceil(Math.max(0, itemCount) / pageSize));
}

export function paginate<T>(items: T[], requestedPage: number, pageSize: number): PaginationResult<T> {
  const totalPages = pageCount(items.length, pageSize);
  const page = Math.min(totalPages, Math.max(1, Math.trunc(requestedPage) || 1));
  const start = (page - 1) * Math.max(1, pageSize);
  return {
    items: pageSize > 0 ? items.slice(start, start + pageSize) : [],
    page,
    totalPages,
  };
}
