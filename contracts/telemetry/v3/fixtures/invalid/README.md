Invalid fixtures are classified by stable error code: duplicate key, unsupported schema,
leading-zero/overflow/negative uint64, count mismatch, duplicate record ID, hash mismatch,
body too large, record too large, depth exceeded, string too long, unknown kind/sensor/
stream, non-finite number, malformed profile reference, inconsistent record identity,
missing source IDs, strict typed payloads, and unexpected field. All thirty JSON fixtures are executed by
the manifest-driven Go corpus test. The 129-record case exercises the semantic batch
count guard; body, record, depth, and string limits remain independently covered. The
parser must report each category without truncating or partially accepting the batch.
