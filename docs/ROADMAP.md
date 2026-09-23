# Roadmap

Ideas raised but not scheduled.

- **JSONPath queries** (decision 14): a `jsonk_query(string $json, string
  $path)` could wrap simdjson's native `.at_pointer()` (RFC 6901) and
  `.at_path()`/`.at_path_with_wildcard()` (an RFC 9535 subset: key names,
  array indices, `*`). Filters, slices and recursive descent (`..`) would
  need real work beyond that.
