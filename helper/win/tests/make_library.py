"""make_library.py OUT.db: a plain SQLite stand-in for rekordbox's master.db (the tables and columns
helper/library.c reads; the test sqlite3.dll accepts any key)."""
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.executescript("""
CREATE TABLE djmdPlaylist (ID VARCHAR PRIMARY KEY, ParentID VARCHAR, Name VARCHAR, Attribute INTEGER, Seq INTEGER, rb_local_deleted INTEGER DEFAULT 0);
CREATE TABLE djmdSongPlaylist (ID VARCHAR PRIMARY KEY, PlaylistID VARCHAR, ContentID VARCHAR, TrackNo INTEGER, rb_local_deleted INTEGER DEFAULT 0);
CREATE TABLE djmdContent (ID VARCHAR PRIMARY KEY, FolderPath VARCHAR);
INSERT INTO djmdPlaylist VALUES ('1','root','Folder F',1,1,0),('2','1','Set A',0,1,0),('3','root','Set B',0,2,0),
  ('4','root','Smart',4,3,0),('5','root','Gone',0,4,1),('6','1','Zed',0,2,0);
INSERT INTO djmdContent VALUES ('10','C:\\Music\\a.wav'),('11','C:/Music/b.flac'),('12','soundcloud:123'),('13','C:\\M\u00fasica\\\u00e9.mp3');
INSERT INTO djmdSongPlaylist VALUES ('s1','2','11',2,0),('s2','2','10',1,0),('s3','2','10',3,0),('s4','2','12',4,0),('s5','2','13',5,0),('s6','2','10',6,1),('s7','3','10',1,0);
""")
db.commit()
