// When the watcher may ask (app/Sources/Watcher.swift's watcherSettle, as one decision per look: the
// Windows watcher looks once a minute instead of waiting in a loop).
namespace RBStemsPlus;

public enum Settle { Wait, Go, Skip }

public static class WatcherLogic
{
    /// The most seconds the watcher waits for RB Stems Plus itself to stop being busy.
    public const int AppBusyMaxSeconds = 600;

    /// What a look finds, and what to do:
    /// - Wait: rekordbox's updater or Pioneer's installer is running (as long as that takes: its window
    ///   may stay open), the app is busy (up to AppBusyMaxSeconds), or rekordbox is open (reinstalling
    ///   needs it closed, and the user may be playing). In any order and more than once: the updater
    ///   quits rekordbox after it starts, and rekordbox may reopen after it.
    /// - Skip: the app has been busy for too long (it will say what it needs itself), or an uninstall
    ///   was left half-done (the next launch offers to finish it, never to reinstall what it removed).
    /// - Go: nothing is changing rekordbox, and it is closed.
    /// busySeconds: how long the app has been seen busy, in a row. halfUninstalled: the id of an
    /// uninstall left half-done, or null; it is only asked for once nothing else is going on.
    public static Settle Decide(bool updating, bool appBusy, bool rekordboxOpen, int busySeconds, Func<string?> halfUninstalled, out string? why)
    {
        why = null;
        if (updating) { why = "an update is running: waiting until it ends"; return Settle.Wait; }
        if (appBusy)
        {
            if (busySeconds < AppBusyMaxSeconds) { why = "RB Stems Plus is busy: waiting"; return Settle.Wait; }
            why = "RB Stems Plus is still busy: not asking";
            return Settle.Skip;
        }
        if (halfUninstalled() is { } id) { why = $"an uninstall didn't finish ({id}): not asking"; return Settle.Skip; }
        if (rekordboxOpen) { why = "rekordbox is open: waiting until it quits"; return Settle.Wait; }
        return Settle.Go;
    }
}
