Invalid fixtures are classified by stable error code: duplicate key, unsupported schema,
leading-zero/overflow/negative uint64, count mismatch, duplicate record ID, hash mismatch,
body too large, record too large, depth exceeded, string too long, unknown kind/sensor/
stream, non-finite number, malformed profile reference, inconsistent record identity,
missing source IDs, and unexpected field. The parser must report the category without
truncating or partially accepting the batch.
