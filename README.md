# Electric Pizza Cooker El-Pico
El-Pico, This is a programmable lectric Pizza Cooker (Oven)!
\
![Screenshot](image/image.png)
CADデータのレンダリングイメージ

## Why Pizza ?
おいしいピッツァを食べたい！→ピザ窯をつくろう
\
\
以下のデータを順次公開します。
* 板金用STEPファイル
* 3Dプリント用STLファイル
* ファームウェア
* 部品のリスト
* 配線や組み立ての手順

## 開発にあたって
* おいしいピッツァが焼けること。
* 日本の家庭用コンセントで使えること。（AC100V 15A）
* 窯の天井485℃以上、炉床380~430℃で90秒以内で焼成可能を目指す。

## 実機写真
![Screenshot](image/El-Pico_01.png)


### もっと多くの人を笑顔にしたい
開発のきっかけは、学園祭での飲食ブースの混雑と[Xの@SteveKasuya2さんのポスト](https://x.com/SteveKasuya2/status/1695339494550224910)でのオーブントースター改造ピザ窯で90 秒でピザを焼きあげる様子から、素早い提供を行えるマシンの開発に魅力を感じたことでした。
さらに、震災の報道で見た避難所の様子と[@nanbuwksさんの災害支援でピザ窯で炊き出しの記事](https://qiita.com/nanbuwks/items/adf3fea1b13d262047f9)にある支援活動の情報から、電気式のピザ窯の有用性を感じたのも理由です。

### 食は心身の健康の増進と豊かな人間形成に資する
2005 年には食育基本法が施行され、食育が現在及び将来にわたる健康で文化的な国民の生活と豊かで活力ある社会の実現に寄与するものであること、[SDGｓとも深くつながること](https://www.maff.go.jp/j/syokuiku/network/topics/2022forum.html)、また、生活の中で身近に体験する科学であり、結果をすぐに体感出来ることや新たな創意工夫を発想しやすいことから、プログラム可能なピザ窯開発を教材化しようと考えました。

### 新たな可能性は改良から生まれます。
プログラムを改良することで、新たな料理や素材に合わせた設定を探ったり、より良い制御を試すことが出来ます。
筐体を作り変えたり、断熱材や蓄熱材、遮熱処理などを変更するとどうなるか、より使いやすくするためのアイデアを考えたり、MakersFair や地産地消や防災・減災イベントやコンペなどへの参加の可能性などもありそうです。

### 仕様（ プロトタイプ）
シーズヒーター * 5　合計1323W\
K 熱電対 * 4　（ヒーター * 2、プレート * 2）\
SSR * 2\
セラミック蓄熱プレート*2\
SUS304、A5052、SPCC（粉体塗装）、冷却ファン、スイッチング電源\
Pro Micro を使用し、PID 制御　リミット温度超過、熱電対断線時にはヒーター切\
ヒューズによる過電流保護\
OLED とロータリーエンコーダで設定変更