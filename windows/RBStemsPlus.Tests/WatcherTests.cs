// WatcherLogic.Decide, over the cases of app/Tests/WatcherTests.swift's "waiting until nothing changes rekordbox".
using Xunit;

namespace RBStemsPlus.Tests;

public class WatcherTests
{
    static Settle Look(string state, int busy = 0, string? half = null, string? uninstall = null)
        => WatcherLogic.Decide(state.Contains('u'), state.Contains('b'), state.Contains('o'), busy,
                               () => state.Contains('h') ? "uninstall-all" : uninstall ?? half, out _);

    [Fact] public void NothingGoingOn_Goes() => Assert.Equal(Settle.Go, Look(""));
    [Fact] public void UpdaterRunning_Waits() => Assert.Equal(Settle.Wait, Look("u"));
    [Fact] public void UpdaterWhileRekordboxOpen_Waits() => Assert.Equal(Settle.Wait, Look("uo"));
    [Fact] public void RekordboxOpen_Waits() => Assert.Equal(Settle.Wait, Look("o"));
    [Fact] public void UpdateAndRekordboxInTurns_GoesOnlyWhenNeitherIsThere()
    {
        var looks = new[] { "o", "o", "u", "o", "o", "u", "u", "" };
        Assert.All(looks.Take(looks.Length - 1), l => Assert.Equal(Settle.Wait, Look(l)));
        Assert.Equal(Settle.Go, Look(looks[^1]));
    }
    [Fact] public void InstallerWindowOpenForLong_WaitsAsLongAsItIsThere()
    {
        for (int i = 0; i < 500; i++) Assert.Equal(Settle.Wait, Look("u", busy: 0));
        Assert.Equal(Settle.Go, Look(""));
    }
    [Fact] public void AppBusy_WaitsThenSkipsAfterTenMinutes()
    {
        Assert.Equal(Settle.Wait, Look("b", busy: 0));
        Assert.Equal(Settle.Wait, Look("b", busy: WatcherLogic.AppBusyMaxSeconds - 1));
        var d = WatcherLogic.Decide(false, true, false, WatcherLogic.AppBusyMaxSeconds, () => null, out var why);
        Assert.Equal(Settle.Skip, d);
        Assert.Equal("RB Stems Plus is still busy: not asking", why);
    }
    [Fact] public void HalfDoneUninstall_Skips_OnceNothingElseIsGoingOn()
    {
        var d = WatcherLogic.Decide(false, false, false, 0, () => "uninstall-all", out var why);
        Assert.Equal(Settle.Skip, d);
        Assert.StartsWith("an uninstall didn't finish", why);
        // not looked for while an update runs or the app is busy (it has a cost): the update comes first
        bool asked = false;
        Assert.Equal(Settle.Wait, WatcherLogic.Decide(true, false, false, 0, () => { asked = true; return "x"; }, out _));
        Assert.False(asked);
    }
    [Fact] public void HalfDoneUninstall_WhileRekordboxIsOpen_SkipsRatherThanWaiting()
    {
        // upstream: an uninstall half-done when rekordbox quits: not asking
        Assert.Equal(Settle.Skip, WatcherLogic.Decide(false, false, true, 0, () => "uninstall-all", out _));
    }
    [Fact] public void AppBusyWhenRekordboxQuits_AfterTenMinutes_SkipsLogged()
    {
        var d = WatcherLogic.Decide(false, true, true, 600, () => null, out var why);
        Assert.Equal(Settle.Skip, d);
        Assert.Contains("still busy", why);
    }
}
