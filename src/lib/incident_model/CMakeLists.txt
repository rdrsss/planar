# planar.incident_model — layer-1 shared value types for `planar diagnose`
# and the incident ledger (plan 1132, tech spec 689 § Module boundaries).
#
# It holds the finding, coverage-row, incident and occurrence records, the
# fingerprint and evidence-digest construction, the severity ordinal, the
# check kind, the closed incident-status enum and the fixed finding sort
# order. It has no database access: the diagnose engine, the incident
# ledger's read and write halves and every command handler share these
# types without an engine-to-engine edge (D15/D18, D19). The only
# dependency is `sha256`, for the evidence digest.

planar_module(incident_model
  INTERFACE incident_model.cppm
  SOURCES incident_model.cpp
  DEPENDS sha256)
