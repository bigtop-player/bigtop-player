/// @file StartPanel.h
/// @brief The screen shown when no show is open, or after "Shows...": New
///        show, Open show, Back to the current show (if one is open), and,
///        when a show.json could not be read, its error and the offer to
///        restore the newest loadable backup. Also: Export show... (the open
///        show as a .bigtop.zip) and Import show... (a .bigtop.zip into an
///        empty folder, then opened). Part of the main window, never a modal
///        dialog; the panel is only reachable outside Show mode.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "ShowSession.h"

namespace bigtop::app {

/// @brief New / Open / Restore backup / Export / Import, with the last message in plain text.
/// @note Message thread.
class StartPanel : public juce::Component {
    friend struct AppTestAccess;

public:
    /// @brief Builds the panel.
    /// @param session Session the chosen folder is opened in; must outlive the panel.
    /// @param onOpened Called after a show was opened (also by an import), or
    ///        "Back" was pressed (the owner switches screens).
    StartPanel(ShowSession& session, std::function<void()> onOpened);

    /// @brief Shows why a show did not open, and the restore offer if any.
    /// @param dir The show folder that failed.
    /// @param error Reason, as ShowSession gave it.
    /// @param backups Loadable backups, newest first; empty = no restore offer.
    void showFailure(const juce::File& dir, const std::string& error, std::vector<juce::File> backups);

    /// @brief Says that the last show's folder is gone (moved or deleted), so the
    ///        operator knows why the start panel came up instead of the show.
    /// @param path The stored path of that folder.
    void showMissing(const juce::String& path);

    /// @brief Lays out the buttons (JUCE callback).
    void resized() override;

    /// @brief Shows "Back to <show>" and "Export show..." when a show is open,
    ///        i.e. the panel was opened through "Shows..." (JUCE callback).
    void visibilityChanged() override;

private:
    void choose(bool createNew);
    void tryOpen(const juce::File& dir);
    void exportTo(const juce::File& folder);
    void importFrom(const juce::File& zip, const juce::File& emptyDir);
    void note(const juce::String& text, bool isError);
    void opened();  // clears an old failure, then tells the owner
    void setBusy(bool busy);

    ShowSession& session_;
    std::function<void()> onOpened_;
    juce::Label title_, error_;
    juce::TextButton new_{"New show"}, open_{"Open show"}, export_{"Export show..."}, import_{"Import show..."}, restore_, back_;
    std::unique_ptr<juce::FileChooser> chooser_;   // kept alive while the async chooser runs
    std::unique_ptr<juce::FileChooser> chooser2_;  // second pick of an import (a chooser must not be replaced inside its own callback)
    juce::File failedDir_;
    juce::File backup_;
    // Export and import run here, off the message thread (a show can be gigabytes).
    // Declared last: destroyed first, which waits for a running job.
    juce::ThreadPool pool_{juce::ThreadPoolOptions{}.withThreadName("Bigtop archive").withNumberOfThreads(1)};
};

}  // namespace bigtop::app
