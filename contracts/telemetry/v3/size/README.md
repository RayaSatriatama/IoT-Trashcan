# Serialized-size review

The committed fixtures are measured as exact UTF-8 bytes. The minimal request is
703 bytes and the pretty-print equivalent is 891 bytes. The canonical record fixture
is 466 bytes before its hash field. Typical serialized records are: HX711 860 bytes,
ultrasonic 903 bytes, MQ 689 bytes, DS18B20 975 bytes, SHT31 974 bytes, and an
unavailable result 1,040 bytes. The mixed batch is 1,606 bytes. The near-limit result
is 16,019 bytes and contains typed 16-entry fuzzy memberships, rules, aggregate data,
and a typed safety trace. The frozen ceilings remain 16 KiB per record and 32 KiB per
request. The invalid record-too-large fixture remains an independent regression for
the 16 KiB boundary.
Full source sample IDs, raw evidence, and model traces are never truncated; a future
fixture that exceeds a ceiling is a contract/storage defect.
