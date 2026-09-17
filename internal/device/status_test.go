package device

import "testing"

func TestCompareUsesCanonicalPrecedence(t *testing.T) {
	cases := []struct {
		higher Status
		lower  Status
	}{
		{StatusSafetyAlert, StatusAngkat},
		{StatusAngkat, StatusWaspada},
		{StatusWaspada, StatusDataInvalid},
		{StatusWaspada, StatusOffline},
		{StatusDataInvalid, StatusNormal},
		{StatusOffline, StatusNormal},
	}

	for _, test := range cases {
		t.Run(string(test.higher)+" over "+string(test.lower), func(t *testing.T) {
			if got := Compare(test.higher, test.lower); got <= 0 {
				t.Fatalf("Compare(%q, %q) = %d, want > 0", test.higher, test.lower, got)
			}
		})
	}
}

func TestDataInvalidAndOfflineRemainDistinct(t *testing.T) {
	if StatusDataInvalid == StatusOffline {
		t.Fatal("Data Invalid and Offline must be distinct statuses")
	}
	if got := Compare(StatusDataInvalid, StatusOffline); got != 0 {
		t.Fatalf("Compare(Data Invalid, Offline) = %d, want equal precedence", got)
	}
	if got := Aggregate(StatusOffline, StatusDataInvalid); got != StatusOffline {
		t.Fatalf("Aggregate preserved status = %q, want Offline", got)
	}
}

func TestAggregateReturnsHighestStatus(t *testing.T) {
	if got := Aggregate(StatusNormal, StatusDataInvalid, StatusAngkat, StatusWaspada); got != StatusAngkat {
		t.Fatalf("Aggregate() = %q, want Angkat", got)
	}
}
