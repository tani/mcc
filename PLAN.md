# 再開計画

## 前提

作業はユーザー指示により一時停止中。再開時は `PROGRESS.md`、`docs/extensions.md`、実際の Git 状態とテスト結果を確認する。対象は GNU C11 拡張全体で、OpenMP・OpenACC・インラインアセンブリは除外する。GCC のソースコードは読まず、公開仕様と GCC 実行結果で比較する。

## 優先順

1. 中断直前の `-Wclobbered` を監査する。通常／`-Wextra`／`-Wclobbered`／`-Wno-clobbered`／`-Werror=clobbered` と診断 pragma、`setjmp`・`__builtin_setjmp`・`returns_twice`・`vfork`、volatile、分岐・ループ・到達不能コードを対象に GCC と比較し、自動回帰テストを追加する。誤検出と見逃しを修正する。
2. アトミックの相互運用を設計・検証する。特に非ロックフリー操作で、異なるランタイム、共有ライブラリー、静的実行ファイル間の同期方法を明確にし、依存関係と ABI を確認する。単体実行の成功だけで完了としない。
3. オブジェクトサイズと `access` 解析で、ポインターのエスケープ、別名、複雑な分岐・ループ、異なるオブジェクトへの代入の精度と安全性を改善する。`__builtin_clear_padding` と既存アトミックの残りの仕様・診断も監査する。
4. GCC 公開マニュアルを基に対象の属性、builtin、pragma、警告・オプションを体系的に棚卸しし、未対応を実装する。`malloc`、`pure`、`const`、`unused`、フォーマット診断、`-Wattribute-alias` レベル 2 を含める。既知の未対応を `docs/extensions.md` に反映する。
5. 各変更を対象別に検証し、最後に全テスト、両ターゲット、stage 2 / stage 3 のビルド・検証を実行する。GCC との比較やセルフホストだけで全機能の網羅性を推定しない。

## 主な確認コマンド

```sh
nix develop --offline --no-write-lock-file path:. --command make -j4
nix develop --offline --no-write-lock-file path:. --command make check
nix develop --offline --no-write-lock-file path:. --command make check-gnu-extensions
nix develop --offline --no-write-lock-file path:. --command make check-suite check-strict check-extended
nix develop --offline --no-write-lock-file path:. --command make check-bootstrap
```

ターゲットを指定した GNU 拡張テストの実行方法は `docs/extensions.md` と `tests/check_gnu_extensions.py --help` を確認する。stage 2 / stage 3 の具体的な手順は現在の Makefile・ブートストラップスクリプトを確認して実行する。
