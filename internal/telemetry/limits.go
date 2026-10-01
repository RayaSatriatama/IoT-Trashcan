package telemetry

const (
	MaxBodyBytes   = 32 * 1024
	MaxRecordBytes = 16 * 1024
	MaxRecords     = 128
	// MaxProfileRefsPerRecord is independent from MaxRecords, which only
	// limits the four top-level batch record arrays.
	MaxProfileRefsPerRecord = 16
	MaxJSONDepth            = 12
	MaxFreeStringBytes      = 512
)
