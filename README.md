# ポインター用Input Processor

このmoduleはZMKのinput processor APIを使い、カーソルの回転と加速、layerによるscroll／encoder modeの切り替え、軸snap、一時mode切り替えbehavior、自動mouse layerを提供します。設定のschemaと保存機構はDynamic Settingsが担当します。

本moduleはnemu-link用のZMK構成を前提とします。

## 導入（最小構成）

### west.yml

この例は既存の`west.yml`へ追記します。`sekigon-gonnoc` remoteが既にある場合は重ねて定義しません。nemu-link構成では、互換性のあるZMK forkと`zmk-feature-nemu-link`もmanifestに必要です。

```yaml
manifest:
  remotes:
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
  projects:
    - name: zmk-feature-dynamic-settings
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-input-processor
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
```

revisionには利用するZMK / nemu-link moduleと互換性のあるtagまたはcommitを指定し、manifest更新後に`west update`を実行します。

### .conf

```conf
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_STUDIO_RPC=y
CONFIG_ZMK_DYNAMIC_SETTINGS=y
CONFIG_SETTINGS_NVS=y
CONFIG_ZMK_POINTING=y
CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR=y
```

`CONFIG_SETTINGS_NVS`の代わりに、対象boardで利用するSettings backendを有効にできます。

### overlay

実際の入力deviceとlistener node labelに合わせて`device`を指定します。`DYNAMIC_INPUT_PROCESSOR_POINTER_SETTINGS`はprocessorの既定値をDynamic Settingsへ登録します。

```dts
#include <input/processors.dtsi>
#include <input_processors/zmk-input-processor-pointer-settings.dtsi>

/ {
    pointer: pointer {
        compatible = "zmk,input-processor-pointer";
        #input-processor-cells = <0>;
    };

    DYNAMIC_INPUT_PROCESSOR_POINTER_SETTINGS(
        pointer_settings, "pointer", "keyboard-right", "Pointer", pointer, "owner")
};

&pointer_listener {
    compatible = "zmk,input-listener";
    device = <&pointer_input>;
    input-processors = <&pointer>;
};
```

`pointer_listener`と`pointer_input`はkeyboard側で宣言したnode labelに置き換えてください。既にlistener nodeがある場合はそのnodeへprocessorを追加します。

## 追加設定

pointerごとにnodeを1つ宣言します。`encoder-sensor-indices`には対象数の2倍のindexを指定し、各対象のX/Y軸をZMKの`zmk,keymap-sensors` listへ対応付けます。processorは指定indexへ通常の`zmk_sensor_event`回転tickを出力します。CW／CCWの動作はkeymapの`sensor-bindings`や`&sensor_rotate`などのbehaviorで選びます。

encoder mode(≒4方向ジェスチャ)を使う場合には`CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR_POINTER_ENCODER_COUNT`を指定します（既定値0、範囲0〜8）。
encoder modeでは、カーソルの4方向の動きを、2つの仮想エンコーダのCW/CCWの動きに割り当てられます。
追加したエンコーダの動作も動的に切り替えるためには、`zmk-feature-dynamic-encoder`モジュールの追加も必要です。

次の`prj.conf`はencoder対象を2つ有効にします。

```conf
CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR=y
CONFIG_ZMK_DYNAMIC_INPUT_PROCESSOR_POINTER_ENCODER_COUNT=2
```

```dts
#include <input/processors.dtsi>
#include <dt-bindings/input/input-event-codes.h>
#include <input_processors/zmk-input-processor-pointer-settings.dtsi>

/ {
    pointer: pointer {
        compatible = "zmk,input-processor-pointer";
        #input-processor-cells = <0>;
        /* ENCODER_COUNT*2個分のエンコーダ番号を指定してください。対象0のX/Y、その後に対象1のX/Yの順で指定します。 */
        encoder-sensor-indices = <0 1 2 3>;
        /* 以下の設定はオプションで省略可能です。省略しても後からnemu-link Studioで変更できます */
        type = <INPUT_EV_REL>;
        x-code = <INPUT_REL_X>;
        y-code = <INPUT_REL_Y>;
        rotation-deg = <0>;
        fine-layers = <0>;
        fine-coef = <1>;
        rough-layers = <0>;
        rough-coef = <1>;
        shift-point-0 = <0>;
        shift-point-1 = <0>;
        shift-point-2 = <0>;
        shift-rate-0 = <100>;
        shift-rate-1 = <100>;
        shift-rate-2 = <100>;
        scroll-layers = <0>;
        scroll-divisor = <1>;
        encoder-layers = <0>;
        encoder-snap-layers = <0>;
        encoder-divisor = <50>;
        aml-layer = <1>;
        aml-timeout-ms = <400>;
        aml-debounce-ms = <25>;
        aml-threshold = <10>;
        aml-delay-ms = <200>;
    };

    /* ENCODER_COUNTと同じ数の仮想エンコーダ紐づけ設定 */
    pointer_scroll: pointer_scroll {
        compatible = "zmk,behavior-pointer-scroll";
        #binding-cells = <0>;
        processor = <&pointer>;
    };
    pointer_encoder: pointer_encoder {
        compatible = "zmk,behavior-pointer-encoder";
        #binding-cells = <1>;
        processor = <&pointer>;
    };

    /* キーボード右側に搭載したポインタを処理し、nemu-linkドングルと設定を共有する場合 */
    DYNAMIC_INPUT_PROCESSOR_POINTER_SETTINGS(
        pointer_settings, "pointer-right", "keyboard-right", "Pointer", pointer, "shared")
};

&pointer_listener {
    compatible = "zmk,input-listener";
    device = <&trackball_input_split>;  // 実際の構成に合わせて指定
    input-processors = <&pointer>;      // 手前にxy_transformなど他のInput processorを入れることもできる
};
```

割り当てたsensor indexはすべてkeymapのsensor listに存在し、encoder modeを使うlayerでsensor bindingが設定されている必要があります。

押している間だけscroll modeにするには`&pointer_scroll`を使います。encoder対象N（0始まり、設定数未満）を一時選択するには`&pointer_encoder N`を使います。複数のencoder behaviorを同時に押した場合は、最も大きい対象番号が選ばれます。

上記の2対象構成では、対象を一時選択するkeyに`&pointer_encoder 0`と`&pointer_encoder 1`を割り当てます。対象数を3に変更する場合は、`encoder-sensor-indices`に6個のsensor indexを指定し、対象番号0、1、2を使います。

### layer設定と優先順位

各layer設定はlayer IDのbit maskです。設定の判定には、有効なlayerのうち最上位のlayerだけを使います。下位layerも有効な場合でも、そのlayer向けのmask設定は適用されません。同じ最上位layerが複数のmaskに含まれる場合は、次の優先関係になります。

- mode選択ではencoderがscrollより優先されます。同じlayerが`encoder-layers`と`scroll-layers`の両方に含まれる場合はencoder modeになります。一時encoder／scroll behaviorもlayer設定と合わせて評価され、encoderが優先されます。
- 一時encoder behaviorが複数押されている場合は、対象番号が最大のものを選びます。
- FineとRoughは互いに優先しません。同じ最上位layerが`fine-layers`と`rough-layers`の両方に含まれる場合は、両方の係数を適用します。
- `encoder-snap-layers`はencoder mode中の軸snapを選びます。mode選択の優先順位には影響しません。

### カーソル倍率と軸snap

shift pointはraw input countを使います（設定値1につき1 count）。shift rateはカーソル倍率（%）で、100が基準です。最初のshift pointを0にするとcurveを無効にし、全域でRate 3を使います。Point 1未満はRate 1、Point 1からPoint 2まではRate 1からRate 2へ線形補間し、Point 2からPoint 3まではRate 2からRate 3へ線形補間します。Point 3以降はRate 3です。

Point 2はPoint 1より大きくしてください。Point 3を指定する場合はPoint 2より大きくします。Point 3が0ならgraph範囲の終端（255）を使います。Point 2が0ならPoint 1からPoint 3までRate 1からRate 3へ直接補間します。Q16 scalerは各軸の端数をreport間で保持し、rotation係数は設定変更時にQ15でcacheします。mouseとscrollの出力はint16範囲に飽和させます。軸snapにはQMKの履歴ルール（5 sample、500 ms timeout、4:1の方向比）を使います。

### 自動mouse layer（AML）

AMLはcursor scaling前のraw sensor countを使います。しきい値に達すると指定layerを有効にし、timeout、debounce、key入力後のdelayを適用します。このprocessor自身が有効にしたlayerだけを解除し、locked layerは解除しません。ZMKのlayer有効化は参照count方式ではないため、同じAML layerを別の一時layer ownerにも割り当てないでください。

### Dynamic Settingsの設定

設定はprocessor instanceごとに独立しています。各instanceについて`zmk-input-processor-pointer-settings.dtsi`をincludeし、`DYNAMIC_INPUT_PROCESSOR_POINTER_SETTINGS(...)`を1回呼び出します。最後の引数でsettings scope（`"owner"`または`"shared"`）を選びます。processorを実行する各firmwareで同じinstanceとshared scopeを使うと、一時設定と永続設定がnemu-linkのshared settingsを通じて同期されます。このmacroはprocessor nodeの既定値から、cursor、scroll、encoder、AMLの各runtime設定を宣言します。

buildではZMK pointingとDynamic Settingsを有効にしてください。
