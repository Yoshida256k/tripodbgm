// テスト用のMP3（data/test.mp3）を読み出し専用データとしてフラッシュに埋め込む
    .section .rodata.test_mp3, "a"
    .balign 4
    .global test_mp3_start
test_mp3_start:
    .incbin "data/test.mp3"
    .global test_mp3_end
test_mp3_end:
