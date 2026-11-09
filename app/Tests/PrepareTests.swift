// Preparing playlists (Prepare.swift, PrepareView.swift), with fake data: the checks before
// preparing, the bridge's kept=1, rekordbox's quality setting, the helper's lines (playlists, an
// estimate, a run), the kept sets on disk, the tree of playlists and the words the window shows.
import Foundation

func prepareTests() {
    // MARK: the checks before preparing
    let ok = PrepareFacts(cacheFolder: "/x")
    check(prepareProblems(ok).isEmpty, "everything in place: no problem")
    var f = ok; f.appleSilicon = false
    check(prepareProblems(f) == ["Preparing playlists needs a Mac with Apple silicon."], "an Intel Mac: that alone")
    f = ok; f.cacheInstalled = false
    check(prepareProblems(f).first?.hasPrefix("Install Stems Cache first") == true, "no Stems Cache: install it first")
    f = ok; f.bridgeKeeps = false
    check(prepareProblems(f) == ["Reinstall Stems Cache: this installation can't keep prepared playlists."], "a bridge without kept=1: reinstall")
    f = ok; f.ortThere = false
    check(prepareProblems(f).first?.hasPrefix("Reinstall Stems Cache") == true, "no copy of Pioneer's ONNX Runtime: reinstall")
    f = ok; f.quality = 1
    check(prepareProblems(f).first?.contains("Prioritize speed") == true, "Prioritize speed: said, with where to change it")
    f = ok; f.quality = 0
    check(prepareProblems(f).isEmpty, "Prioritize sound quality: fine")
    f = ok; f.enabled = false
    check(prepareProblems(f) == ["Stems Cache is turned off in config.ini (enabled=0)."], "Stems Cache off in config.ini")
    f = ok; f.configUnreadable = "a NUL byte"; f.enabled = false
    check(prepareProblems(f) == ["Stems Cache can't read its settings (config.ini: a NUL byte)."], "config.ini unreadable: that, not enabled=0")
    f = ok; f.rekordboxOK = false; f.rekordboxVersion = "7.2.20.0001"
    check(prepareProblems(f).first?.hasPrefix("Preparing doesn't support rekordbox 7.2.20 yet.") == true, "an unlisted rekordbox: not supported yet")
    f = ok; f.cacheLook = .limited; f.cacheTip = "Stems Cache starts saving stems once rekordbox has downloaded its STEMS Engine."
    check(prepareProblems(f) == [f.cacheTip], "Stems Cache not saving now: its light's reason")
    f = ok; f.plusOn = true; f.modelSHA = "ab" + String(repeating: "0", count: 62)
    check(prepareProblems(f).first?.contains("reinstall Demucs v4") == true, "an unknown model in rekordbox with Demucs v4 on: reinstall it")
    f.modelSHA = ourModel
    check(prepareProblems(f).isEmpty, "our model: fine")
    f = ok; f.cacheFolder = nil
    check(prepareProblems(f) == ["Where Stems Cache saves stems can't be used now."], "no folder in use: said")

    // MARK: the bridge's line and rekordbox's setting
    let line = { (s: String) in Data(("xx rbstems bridge: yy rbstems-cache: " + s + "; zz").utf8) }
    check(bridgeSaysKept(data: line("rebuild=1 location=1 kept=1 pioneer=\(ourModel)")), "kept=1 before pioneer=: keeps")
    check(!bridgeSaysKept(data: line("rebuild=1 location=1 pioneer=\(ourModel)")), "no kept=1: doesn't keep")
    check(!bridgeSaysKept(data: line("rebuild=1 pioneer=kept=1")), "kept=1 after pioneer= isn't a token")
    check(!bridgeSaysKept(data: Data("rbstems-cache: kept=1;".utf8)), "without the bridge's own text: no")
    try? FileManager.default.createDirectory(atPath: tmp + "/prep", withIntermediateDirectories: true)
    let settings = tmp + "/prep/rekordbox3.settings"
    try? "<PROPERTIES><VALUE name=\"PartAnalysis\" val=\"1\"/><VALUE name=\"PartAnalysisQuality\" val=\"1\"/></PROPERTIES>".write(toFile: settings, atomically: true, encoding: .utf8)
    check(stemsQuality(settings) == 1, "PartAnalysisQuality read (not the setting named like it)")
    check(stemsQuality(tmp + "/prep/none") == nil, "no settings file: unknown")

    // MARK: the helper's lines
    let pl = parsePlaylists("""
        {"playlist":"1","parent":"root","name":"Folder","kind":"folder","tracks":0}
        {"playlist":"2","parent":"1","name":"Friday \\"Late\\"","kind":"playlist","tracks":12}
        {"playlist":"3","parent":"root","name":"Smart","kind":"intelligent","tracks":0}
        not json
        """)
    check(pl.count == 3 && pl[1].name == "Friday \"Late\"" && pl[1].tracks == 12 && pl[1].canPrepare && !pl[0].canPrepare && !pl[2].canPrepare,
          "playlists parsed, quotes unescaped, only plain playlists preparable: \(pl)")
    let tree = playlistTree(pl)
    check(tree.count == 2 && tree[0].children.map { $0.p.id } == ["2"], "the tree: the playlist under its folder")

    let est = parseEstimate("""
        {"file":"/m/a.flac","result":"ready","entries":6,"gb":0.015}
        {"file":"/m/b.m4a","result":"prepare","seconds":300.0,"entries":56,"gb":0.156,"missing":2}
        {"file":"/Users/x/Documents/c.mp3","result":"skipped","why":"macOS is asking","access":"asking","folder":"Documents"}
        {"estimate":3,"ready":1,"prepare":1,"skipped":1,"entries":56,"runs":28,"time_s":70,"need_gb":0.156,"kept_gb":2.000,"share_gb":15.000,"max_gb":20,"cache_gb":4.000,"free_gb":100.0,"fits":true,"fit_files":3,"max_gb_needed":2.875,"disk_ok":true,"asking":1,"unchecked":0,"folders":["Documents"]}
        """)
    check(est?.tracks.count == 3 && est?.ready == 1 && est?.prepare == 1 && est?.asking == 1 && est?.folders == ["Documents"] && est?.fits == true,
          "an estimate parsed")
    check(est?.tracks[1].missing == 2 && est?.tracks[2].access == "asking", "its tracks' fields")
    check(parseEstimate("{\"file\":\"/a\",\"result\":\"ready\"}") == nil, "an estimate without its last line: none")

    // MARK: the tracks' rows
    var rows = parseTrackList("{\"file\":\"/m/a \\\"x\\\".flac\"}\nnot json\n{\"file\":\"/m/b.m4a\"}\n")
    check(rows.map { $0.name } == ["a \"x\".flac", "b.m4a"] && rows.allSatisfy { $0.result.isEmpty }, "--tracks' lines: the tracks, in order, nothing said about them: \(rows)")
    check(parseTrackList("").isEmpty, "a playlist without tracks: no rows")
    check(resultText(rows[0], checking: true) == "Checking…", "no result, a check under way: Checking…")
    check(resultText(rows[0], checking: false) == "Not checked", "no result, no check: Not checked")
    check(place(TrackLine(file: "/m/b.m4a", result: "ready"), in: &rows) == 1 && rows[1].result == "ready" && rows.count == 2, "a result goes in its track's row")
    check(place(TrackLine(file: "/m/c.mp3", result: "prepare"), in: &rows) == 2 && rows.count == 3, "a track the rows don't have: a new row at the end")
    check(resultText(rows[1], checking: true) == "Ready", "a result that came during the check is shown")
    check(place(TrackLine(file: "/m/c.mp3", result: "ready"), in: &rows, hint: 2) == 2 && rows[2].result == "ready", "the row hinted at, when it is the track's")
    check(place(TrackLine(file: "/m/b.m4a", result: "skipped"), in: &rows, hint: 0) == 1 && rows[1].result == "skipped", "a wrong hint: still its own row")
    check(place(TrackLine(file: "/m/x.mp3", result: "ready"), in: &rows, hint: 99) == 3, "a hint past the end: a new row")
    var forms = [TrackLine(file: "/m/caf\u{e9}.mp3"), TrackLine(file: "/m/cafe\u{301}.mp3")]
    check(place(TrackLine(file: "/m/cafe\u{301}.mp3", result: "ready"), in: &forms) == 1 && forms[0].result.isEmpty, "two paths that differ only in Unicode form: two rows")
    check(notChecked(rows).map { $0.file } == rows.map { $0.file } && notChecked(rows).allSatisfy { $0 == TrackLine(file: $0.file) }, "a new check: the same rows, nothing left of the last one")
    check(parseEstimate("{\"refused\":\"5 playlists are kept already: stop keeping one first\"}")?.refused?.hasPrefix("5 playlists") == true, "a refusal")
    if let e = est {
        let s = estimateSummary(e)
        check(s.hasPrefix("1 track ready, 1 to prepare (5 min of music), 1 skipped. About 160 MB and 1 min."), "the summary's words: \(s)")
        check(s.contains("2.2 GB of the 15.0 GB") && s.contains("20 GB limit"), "the summary's share: \(s)")
    }

    check(preparedNote(TrackLine(written: 6)) == "separated now", "a track separated now")
    check(preparedNote(TrackLine(already: 6)) == "already saved", "a track saved before")
    check(preparedNote(TrackLine(written: 2, already: 4)) == "partly saved already, the rest separated now", "partly saved before")
    check(preparedNote(TrackLine(written: 5, refused: 1)) == "separated now, some parts couldn't be saved", "a part refused")
    check(preparedNote(TrackLine(refused: 6)) == "couldn't be saved: rekordbox separates it when loaded", "every part refused: not \"already saved\"")
    check(preparedNote(TrackLine()) == "silent: nothing to save", "only silence: not \"already saved\"")
    check(parseRunLine("{\"at\":2,\"files\":10,\"chunk\":3,\"chunks\":30}") == .progress(file: 2, files: 10, chunk: 3, chunks: 30), "a progress line")
    check(parseRunLine("{\"room\":4,\"cache_gb\":17.500}") == .room(removed: 4, cacheGB: 17.5), "a room line")
    check(parseRunLine("{\"done\":5,\"stopped\":\"stopped\",\"runs\":3,\"run_s\":5.0,\"kept\":\"7\",\"until\":1,\"kept_gb\":0.5,\"manifest_error\":\"x\"}")
          == .done(files: 5, stopped: "stopped", keptGB: 0.5, error: "x"), "a done line, its error")
    if case .track(let t)? = parseRunLine("{\"file\":\"/m/a.flac\",\"result\":\"prepared\",\"written\":3,\"already\":1,\"silent\":0,\"refused\":0,\"how\":\"pcm\"}") {
        check(t.written == 3 && t.already == 1 && t.name == "a.flac", "a track line")
    } else { check(false, "a track line") }

    // MARK: the doesn't-fit sheet
    var big = Estimate(); big.fits = false; big.fitFiles = 4; big.maxGB = 20; big.maxGBNeeded = 24.31; big.shareGB = 15; big.keptGB = 12; big.needGB = 6.2
    var d = doesntFitChoices(big, freeGB: 50)
    check(d.buttons == ["Prepare the First 4 Tracks", "Raise the Limit to 24.4 GB", "Cancel"] && d.raiseTo == 24.4, "doesn't fit: the first tracks, a higher limit, cancel: \(d.buttons)")
    d = doesntFitChoices(big, freeGB: 3)
    check(d.buttons == ["Prepare the First 4 Tracks", "Cancel"] && d.message.contains("doesn't have room for a higher limit"), "no room on the disk: no higher limit")
    big.fitFiles = 0
    check(doesntFitChoices(big, freeGB: 3).buttons == ["Cancel"], "nothing fits, no room: cancel only")

    // MARK: the kept sets on disk
    let kd = tmp + "/prep/kept"
    try? FileManager.default.createDirectory(atPath: kd, withIntermediateDirectories: true)
    let entry = ourModel + " " + String(repeating: "a", count: 64)
    let now = Date()
    try? "rbstemsplus-kept 1\nuntil \(Int(now.timeIntervalSince1970) + 3600)\n\(entry)\n\(entry)\n".write(toFile: kd + "/123.kept", atomically: true, encoding: .utf8)
    try? "rbstemsplus-manifest 1\nhelper 1\nmodel \(ourModel)\nplaylist 123\nrate 0 0 4\nfile 1 2 1 0 /m/a.flac\nfile 1 2 1 0 /m/b.flac\n".write(toFile: kd + "/123.manifest", atomically: true, encoding: .utf8)
    try? "rbstemsplus-kept 1\nuntil \(Int(now.timeIntervalSince1970) - 10)\n".write(toFile: kd + "/old.kept", atomically: true, encoding: .utf8)
    try? "nonsense".write(toFile: kd + "/bad.kept", atomically: true, encoding: .utf8)
    try? "x".write(toFile: kd + "/.hidden.kept", atomically: true, encoding: .utf8)
    try? "x".write(toFile: kd + "/no space.kept", atomically: true, encoding: .utf8)
    let sets = keptSets(kd, now: now)
    check(sets.map { $0.name } == ["123", "bad"] && sets[0].entries == 2 && sets[0].files == 2 && sets[0].until != nil && sets[1].until == nil,
          "kept sets: past their date, hidden and badly named ones left out; one the bridge can't read still listed: \(sets)")
    check(stopKeeping("123", dir: kd) && fileType(kd + "/123.kept") == nil && fileType(kd + "/123.manifest") == nil, "stop keeping: the set and its manifest go")
    check(!stopKeeping("../x", dir: kd), "stop keeping: a name with a slash is refused")
    check(keptNameOK("133477215") && keptNameOK("a-b_C") && !keptNameOK("") && !keptNameOK("a b") && !keptNameOK(String(repeating: "a", count: 65)), "set names as the helper allows them")

    // MARK: words
    check(lastRunLine(files: 1, skipped: 0, stopped: "stopped") == "Last time: 1 track done, stopped before the end."
          && lastRunLine(files: 12, skipped: 2, stopped: nil) == "Last time: 12 tracks done, 2 skipped."
          && lastRunLine(files: 3, skipped: 0, stopped: "prepared stems would take more than 75% of the cache's 20 GB limit") == "Last time: 3 tracks done, stopped at the 75% share.",
          "the last run in one line")
    func title(status: Int32 = 0, gotDone: Bool = true, stopped: String? = nil, finished: Int = 3, skipped: Int = 0, error: String? = nil, partial: Bool = false) -> String {
        runEndTitle("Fri", status: status, gotDone: gotDone, stopped: stopped, finished: finished, skipped: skipped, error: error, partial: partial)
    }
    check(title() == "“Fri” is prepared", "a whole run: is prepared")
    check(title(stopped: "prepared stems would take more than 75%") == "“Fri” isn't fully prepared"
          && title(stopped: "stopped") == "“Fri” isn't fully prepared", "a run stopped early: isn't fully prepared")
    check(title(skipped: 1) == "“Fri” isn't fully prepared", "a track skipped: isn't fully prepared")
    check(title(partial: true) == "“Fri” isn't fully prepared", "only the first tracks: isn't fully prepared")
    check(title(error: "the kept set can't be written") == "“Fri” isn't fully prepared", "the set not saved: isn't fully prepared")
    check(title(stopped: "stopped", finished: 0) == "“Fri” couldn't be prepared"
          && title(finished: 0, skipped: 5) == "“Fri” couldn't be prepared", "no track finished: couldn't be prepared")
    check(title(status: 1, gotDone: false) == "“Fri” couldn't be prepared"
          && title(gotDone: false) == "“Fri” couldn't be prepared", "a failed or crashed run: couldn't be prepared")
    check(lastRunLine(files: 12, skipped: 0, stopped: nil, error: "the kept set can't be written") == "Last time: 12 tracks done, the set couldn't be saved (see the log)."
          && lastRunLine(files: 0, skipped: 5, stopped: nil) == "Last time: 0 tracks done, 5 skipped.", "the last run: not saved, or nothing finished")
    do {
        let good = #"{"estimate":1,"ready":1,"prepare":0,"skipped":0}"#
        var calls: [[String]] = []
        let now = checkSet("42", ["--keep", "42"]) { calls.append($0); return (0, good, "") }
        check(now.out == good && calls == [["--keep", "42", "--playlist", "42"]], "a set's check reads the playlist as it is now")
        calls = []
        let gone = checkSet("42", ["--keep", "42"]) { calls.append($0); return $0.contains("--playlist") ? (1, "", "no playlist 42") : (0, good, "") }
        check(gone.out == good && calls == [["--keep", "42", "--playlist", "42"], ["--keep", "42"]], "a playlist that can't be read: the set's own files")
    }
    do {
        let e = parseEstimate("""
            {"file":"/a.mp3","result":"ready"}
            {"file":"/b.mp3","result":"prepare"}
            {"file":"/c.mp3","result":"skipped","why":"gone"}
            {"file":"/Volumes/D/d.mp3","result":"unchecked"}
            {"estimate":4,"ready":1,"prepare":1,"skipped":1,"unchecked":1}
            """)
        check(e.map(notReadyTracks) == ["/b.mp3", "/c.mp3"], "not ready: to prepare or skipped, not an unchecked one")
        check(newlyNotReady(["/b"], noticed: []) && !newlyNotReady(["/b"], noticed: ["/b"]) && !newlyNotReady(["/b"], noticed: ["/b", "/c"])
              && newlyNotReady(["/b", "/d"], noticed: ["/b"]) && !newlyNotReady([], noticed: ["/b"]), "a notification only for a track the last one didn't cover")
        check(setState(parseEstimate(#"{"estimate":0,"ready":0,"prepare":0,"skipped":0}"#)) == "No tracks in the playlist", "an emptied playlist says so")
        check(setRunPattern("42") == "rbsp-prepare .*--until [^ ]+ --keep 42( |$)" && runArgs("42", until: "99", first: nil).prefix(3) == ["--until", "99", "--keep"], "the run a set's check looks for, and not a check")
    }
    check(runArgs("42", until: "99", first: nil) == ["--until", "99", "--keep", "42", "--playlist", "42"]
          && runArgs("42", until: "99", first: 3) == ["--until", "99", "--keep", "42", "--playlist", "42", "--first", "3"],
          "a run names the playlist, also for the first N tracks, so the set knows the rest of it")
    check(noPaths("macOS is asking whether RB Stems Plus may read /Volumes/My Drive: answer its prompt, then check again")
          == "macOS is asking whether RB Stems Plus may read <path>: answer its prompt, then check again", "logs: a drive's path masked")
    check(noPaths("can't read /Users/dj/Music/Café ♫.flac") == "can't read <path>" && noPaths("the file isn't there (moved, renamed, or its drive isn't connected)")
          == "the file isn't there (moved, renamed, or its drive isn't connected)", "logs: a track's path masked, a plain reason kept")
    check(durationText(30) == "under a minute" && durationText(600) == "10 min" && durationText(3900) == "1 h 05 min", "durations")
    check(sizeText(0.156) == "160 MB" && sizeText(2.44) == "2.4 GB", "sizes")
    var r = Estimate(); r.ready = 2; r.prepare = 0; r.skipped = 0
    check(setState(r) == "Ready" && setState(nil) == "Couldn't be checked", "a set's state: ready, or not checked")
    r.prepare = 3; r.skipped = 1
    check(setState(r) == "3 tracks to prepare, 1 can't be read", "a set's state: what changed")
    r.prepare = 0; r.skipped = 0; r.unchecked = 2
    check(setState(r) == "Ready (2 not checked)", "a set's state: unchecked tracks said")
    check(keptSetNotice(r) == nil, "the watcher: a ready set, no notification")
    r.prepare = 1
    check(keptSetNotice(r) == "1 track isn't fully prepared: rekordbox may separate it as you play. Open RB Stems Plus to see why and prepare it again.",
          "the watcher's notification for one track (changed, moved, not finished or its stems removed: the app says which)")
    r.skipped = 2
    check(keptSetNotice(r)?.hasPrefix("3 tracks aren't fully prepared: rekordbox may separate them") == true, "the watcher's notification for three")
}
