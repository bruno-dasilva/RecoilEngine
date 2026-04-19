+++
title = "Running changelog"
[cascade]
  [cascade.params]
    type = "docs"
+++

This is the bleeding-edge changelog since version 2025.06, for **pre-release 2026.06**.

## Caveats

## Features

### RmlUi

- add datamodel support for pairs: `pairs(dm_handle)`
- add datamodel support for ipairs: `dm_handle:__ipairs()`
- support for accessing the underlying datamodel table with `dm_handle.__raw()`
- allow datamodel self-referential assignments such as `dm_handle.property = dm_handle.another_property`
- support for retrieving datamodel property length: `dm_handle.property.__len()`
- fix datamodel array access
- fix `data-value` binds in rml elements
- added `RmlUi.GetDocumentPathRequests(string docPath) -> {"filePath", "filePath", ...}` which tracks all of the files opened by an RmlUi LoadDocument call
- added `RmlUi.ClearDocumentPathRequests(string docPath) -> nil` to clear tracked LoadDocument files

### Misc

- archive cache version 20 → 21.
