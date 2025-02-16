# Electric Pizza Cooker El-Pico
El-Pico, This is a programmable Electric Pizza Cooker (Oven)!
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
* レシピ

## 開発にあたって
* おいしいピッツァが焼けること。
* 日本の家庭用コンセントで使えること。（AC100V 15A）
* 窯の天井485℃以上、炉床380~430℃で90秒以内で焼成可能を目指す。

## 実機写真
![Screenshot](image/El-Pico_01.png)
天井500℃、炉床420℃の設定で90秒の焼成でおいしいピッツァを焼くことが出来ました。

## コンセプト
### もっと多くの人を笑顔にしたい
開発のきっかけは、学園祭での飲食ブースの混雑と[Xの@SteveKasuya2さんのポスト](https://x.com/SteveKasuya2/status/1695339494550224910)でのオーブントースター改造ピザ窯で90 秒でピザを焼きあげる様子から、素早い提供を行えるマシンの開発に魅力を感じたことでした。
さらに、震災の報道で見た避難所の様子と[@nanbuwksさんの災害支援でピザ窯で炊き出しの記事](https://qiita.com/nanbuwks/items/adf3fea1b13d262047f9)にある支援活動の情報から、電気式のピザ窯の有用性を感じたのも理由です。

### 食は心身の健康の増進と豊かな人間形成に資する
2005 年には食育基本法が施行され、食育が現在及び将来にわたる健康で文化的な国民の生活と豊かで活力ある社会の実現に寄与するものであること、[SDGｓとも深くつながること](https://www.maff.go.jp/j/syokuiku/network/topics/2022forum.html)、また、生活の中で身近に体験する科学であり、結果をすぐに体感出来ることや新たな創意工夫を発想しやすいことから、プログラム可能なピザ窯開発を教材化しようと考えました。

### 新たな可能性は改良から生まれます。
プログラムや設計を改良することで、新たな料理や素材に合わせた設定を探ったり、より良い制御を試すことが出来ます。\
筐体を作り変えたり、断熱材や蓄熱材、遮熱処理などを変更するとどうなるか、より使いやすくするためのアイデアを考えたり、MakersFair や地産地消や防災・減災イベントやコンペなどへの参加の可能性などもありそうです。

## 制作にあたって
ヒーター部分はとても高温（750℃程度）になります。\
金属部品の過熱による寸法変化や変形などもあり、それを考慮した配線の長さや部材の選定、専用工具による圧着端子での配線を行ってください。\
有資格者や同等の知識を有する方が組み立てと配線および点検を行い、事故に対応できる環境で監視のもとで使用してください。

### 既知の問題点
* 熱でドアが変形し、左右のロックが片側しかかからなくなる\
→ドアの設計を変更する（設計中）

### 仕様（ プロトタイプ）
シーズヒーター * 5　合計1323W\
K 熱電対 * 4　（ヒーター * 2、プレート * 2）\
SSR * 2\
セラミック蓄熱プレート*2\
板金部品　SUS304、A5052、SPCC（粉体塗装）\
冷却ファン、スイッチング電源

Pro Micro とMax6675・K熱電対を使用し、ヒーターをPID 制御\
リミット温度超過や熱電対断線時にはヒーター切\
ヒューズによる過電流保護\
OLED とロータリーエンコーダで温度設定を変更可能

### 公開データ
板金用STEPデータ\
3Dプリント用STEPデータ\
制御プログラム

### 必要部品
シーズヒーター 350mm 320mmL 350W, 110V 2本\
シーズヒーター 345mm 315mmL 300W, 110V 3本\
セラミックの部品 10個 OD9.8xH11xID6.7　[AliExpress](https://ja.aliexpress.com/item/1005002340926694.html) \
ツインバード 892009 ピザプレート 2枚　[yodobashi](https://www.yodobashi.com/product/100000001002338272/)\
ファイバー断熱材\
アルミホイル\
耐熱電線\
耐熱保護チューブ\
SSR 2個\
SSRヒートシンク\
Max6675 モジュール 4個\
K熱電対 2個\
K熱電対 細 2個\
ProMicro\
タクトスイッチ\
パネルマウントUSB延長ケーブル\
スイッチング電源\
ステンレスカラー\
PCファン\
スイッチ\
OLED\
ヒューズボックス\
圧着端子\
組端子台\
DINレール\
DINレール固定金具\
電源コード\
ステンレストラスねじ\
ステンレス針金\
ねじ脚