#pragma once

#include <QString>

namespace mbs::presentation::storage {

[[nodiscard]] QString project_root();
[[nodiscard]] QString database_file();
[[nodiscard]] QString artifact_root();
[[nodiscard]] QString staging_root();
[[nodiscard]] QString simulation_run_root();
[[nodiscard]] QString gui_cache_root();
[[nodiscard]] QString scratch_root();
[[nodiscard]] QString temporary_workspace();
[[nodiscard]] QString export_data_root();
[[nodiscard]] QString mbs_export_file();
[[nodiscard]] QString demo_export_file();
[[nodiscard]] QString materials_export_file();

} // namespace mbs::presentation::storage
