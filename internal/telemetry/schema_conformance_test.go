package telemetry

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/santhosh-tekuri/jsonschema/v6"
)

type sigapSchemaLoader struct {
	root string
}

func (l sigapSchemaLoader) Load(rawURL string) (any, error) {
	const prefix = "https://sigap.local/contracts/telemetry/v3/"
	if strings.HasPrefix(rawURL, prefix) {
		path := filepath.Join(l.root, strings.TrimPrefix(rawURL, prefix))
		file, err := os.Open(path)
		if err != nil {
			return nil, err
		}
		defer file.Close()
		return jsonschema.UnmarshalJSON(file)
	}
	return jsonschema.FileLoader{}.Load(rawURL)
}

func compileTelemetrySchema(t *testing.T) *jsonschema.Schema {
	t.Helper()
	root, err := filepath.Abs("../../contracts/telemetry/v3/schema/batch.schema.json")
	if err != nil {
		t.Fatal(err)
	}
	compiler := jsonschema.NewCompiler()
	compiler.UseLoader(sigapSchemaLoader{root: filepath.Dir(root)})
	schema, err := compiler.Compile(root)
	if err != nil {
		t.Fatalf("compile telemetry schema: %v", err)
	}
	return schema
}

func TestAllTelemetrySchemasCompile(t *testing.T) {
	root, err := filepath.Abs("../../contracts/telemetry/v3/schema")
	if err != nil {
		t.Fatal(err)
	}
	files, err := filepath.Glob(filepath.Join(root, "*.schema.json"))
	if err != nil {
		t.Fatal(err)
	}
	// filepath.Glob does not recurse, so include the nested sample directory
	// explicitly while keeping the test deterministic.
	files = append(files, mustGlobSchemas(t, filepath.Join(root, "sample"))...)
	for _, path := range files {
		path := path
		t.Run(filepath.Base(path), func(t *testing.T) {
			compiler := jsonschema.NewCompiler()
			compiler.UseLoader(sigapSchemaLoader{root: root})
			if _, err := compiler.Compile(path); err != nil {
				t.Fatalf("compile %s: %v", path, err)
			}
		})
	}
}

func mustGlobSchemas(t *testing.T, root string) []string {
	t.Helper()
	files, err := filepath.Glob(filepath.Join(root, "*.schema.json"))
	if err != nil {
		t.Fatal(err)
	}
	return files
}

func decodeSchemaInstance(t *testing.T, path string) any {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	instance, err := jsonschema.UnmarshalJSON(file)
	if err != nil {
		t.Fatalf("decode %s: %v", path, err)
	}
	return instance
}

func TestValidFixturesMatchJSONSchema(t *testing.T) {
	manifest := loadFixtureManifest(t)
	schema := compileTelemetrySchema(t)
	root := "../../contracts/telemetry/v3/fixtures/valid"
	for _, name := range manifest.Valid {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(root, name+".json")
			if err := schema.Validate(decodeSchemaInstance(t, path)); err != nil {
				t.Fatalf("schema rejected valid fixture: %v", err)
			}
		})
	}
}

func TestStructuralInvalidFixturesFailJSONSchema(t *testing.T) {
	schema := compileTelemetrySchema(t)
	root := "../../contracts/telemetry/v3/fixtures/invalid"
	for _, name := range []string{
		"wrong-raw-shape",
		"wrong-converted-shape",
		"malformed-result-processed",
		"malformed-fuzzy-trace",
		"malformed-safety-trace",
		"unknown-sensor",
		"unknown-stream",
		"ds18b20-short-scratchpad",
		"ambient-result",
	} {
		t.Run(name, func(t *testing.T) {
			path := filepath.Join(root, name+".json")
			if err := schema.Validate(decodeSchemaInstance(t, path)); err == nil {
				t.Fatal("schema accepted structural invalid fixture")
			}
		})
	}
}
