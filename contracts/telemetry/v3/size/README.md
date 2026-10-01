# Serialized-size review

The committed fixtures are measured as exact UTF-8 bytes. The minimal request is
703 bytes and the pretty-print equivalent is 891 bytes. The canonical record fixture
is 466 bytes before its hash field. Current compact record sizes are:

| Record | Bytes |
| --- | ---: |
| HX711 typical | 712 |
| HX711 timeout, `raw:null` | 549 |
| Ultrasonic typical | 750 |
| Ultrasonic timeout, `raw:null` | 542 |
| MQ typical | 540 |
| MQ disconnected, `raw:null` | 547 |
| DS18B20 complete | 827 |
| DS18B20 CRC error | 669 |
| SHT31 complete | 826 |
| SHT31 CRC error | 648 |
| FIS unavailable result | 885 |
| Safety unavailable result | 889 |
| Mixed batch body | 1,606 |
| Near-limit typed result record | 16,012 |

The near-limit result contains typed 16-entry fuzzy memberships, rule/output evidence,
aggregate output identity, and a score-free typed safety trace. The frozen ceilings
remain 16 KiB per record and 32 KiB per request. The invalid record-too-large fixture
remains an independent regression for the 16 KiB boundary.
Full source sample IDs, raw evidence, and model traces are never truncated; a future
fixture that exceeds a ceiling is a contract/storage defect.
