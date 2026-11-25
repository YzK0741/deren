using System;
using deren.scripting;

namespace deren.editor;

internal sealed unsafe partial class editor_host {
    private static bool requested_project_check;

    /// <summary>
    /// Runs the shared project check and reports every problem on its own log line. The editor button and the
    /// <c>--check-project</c> command use the same report, so "checks clean here" means "packages clean" for
    /// the export tool that reads these lines.
    /// </summary>
    private project_report run_project_check() {
        var report = project_validator.validate_in_editor(managed_bootstrap.project_directory);
        foreach (var problem in report.problems) {
            host_logger.write($"Project check: {problem.describe()}");
        }
        host_logger.write(report.summary());
        return report;
    }

    private void check_project_from_editor() {
        var report = run_project_check();
        notice = report.summary();
        if (ui == null) { return; }
        ui?.set_text("project_scene_message", report.ok
            ? report.summary()
            : $"{report.summary()} First: {report.problems[0].describe()}");
    }

    private script_status run_project_check_command() {
        return run_project_check().ok ? script_status.success : script_status.script_failure;
    }
}
