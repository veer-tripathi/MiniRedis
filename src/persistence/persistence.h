#pragma once

// ---------------------------------------------------------------------------
// Persistence module — placeholder for Day N
// ---------------------------------------------------------------------------
//
// Planned interface (AOF / RDB style):
//
//   bool persist_load(const char *path);   // restore state on startup
//   void persist_append_cmd(const std::vector<std::string> &cmd);  // AOF write
//   bool persist_snapshot(const char *path);   // RDB dump
