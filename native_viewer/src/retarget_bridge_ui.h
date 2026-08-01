#pragma once

#include "skrtg/viewer/batch_retarget.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skrtg::viewer
{
class RetargetBridgeUi
{
public:
    explicit RetargetBridgeUi(
        const std::filesystem::path& ViewerExecutable);
    ~RetargetBridgeUi();
    RetargetBridgeUi(RetargetBridgeUi&&) noexcept;
    RetargetBridgeUi& operator=(RetargetBridgeUi&&) noexcept;
    RetargetBridgeUi(const RetargetBridgeUi&) = delete;
    RetargetBridgeUi& operator=(const RetargetBridgeUi&) = delete;

    void Open();
    void OpenBatch();
    void OpenSkrvPicker();
    void OpenExportDialog(
        const std::filesystem::path& SourceFbx,
        const std::filesystem::path& ProtectedPackageDirectory,
        const std::string& SuggestedFileName,
        const std::string& ExpectedSha256);
    void Draw();
    void DrawOperationStackWindow(bool* Open);
    void Poll();
    bool IsOpen() const;
    bool IsRunning() const;
    int CompletedRunCount() const;
    std::vector<BatchReviewAnimation>
        ReviewableBatchAnimations() const;
    void SetSelectedBatchReviewJobIndex(std::size_t JobIndex);
    std::optional<std::filesystem::path> ConsumeCompletedPackage();
    std::optional<std::filesystem::path> ConsumeRequestedPackage();
    bool SpinePelvisFollowEnabled() const;
    bool SourceMotionFootLockEnabled() const;
    void SetSpinePelvisFollowEnabled(bool Enabled);
    void SetSourceMotionFootLockEnabled(bool Enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> State;
};
} // namespace skrtg::viewer
