Мод для GTA San Andreas 1.0 US, с которым сюжет можно пройти вдвоём, втроём или вчетвером по сети.

## Что нового

- **Скины, клавиша F4.** В окне скинов обычные люди игры по вкладкам, 266 скинов из паков других авторов (новые члены Families, Ballas, Vagos и Aztecas, банды Сан-Фиерро и Лас-Вентураса, байкеры, бета-версии прохожих и вырезанные из игры прохожие) и ваши свои скины из папки `coop\skins`. Надетый скин видят все игроки. Он остаётся после загрузки сохранения и в катсценах: хост играет роль CJ в своём скине и двигает губами, а напарники стоят рядом в своих. В скине персонаж ходит походкой своего человека и говорит его голосом.
- **Голос героя в скине.** В катсценах и сюжетных диалогах у героя в скине свой голос, женский или мужской по скину, а имя CJ не звучит ни в речи, ни в субтитрах. Голоса лежат в архиве в папке `coop\voices`, поэтому архив стал около 520 МБ. Без этой папки звучит голос CJ.
- **На борт без мест, клавиша G.** В Shamal и других самолётах без пассажирских мест, а ещё в вертолётах и лодках, где мест не осталось, клавиша G сажает на борт к водителю. На лодке у каждого своё сиденье, а с видом от оружия игрок встаёт и стреляет. В самолёте первый сидит рядом с пилотом. Вид меняется клавишей смены камеры.
- **Кого ждёт миссия.** Хост в маркере миссии видит ники тех, кого ждут, а клавиша Y начинает миссию сразу и переносит отставших к нему. Если напарник встал в маркер первым, хост по Y сразу оказывается рядом.
- **Цвет игрока.** Цвет стрелки, который игрок выбирает в окне F9 или командой `/blip`, теперь и у его ника в чате и над головой, у его метки на карте и у пути до неё на радаре.
- **Геймпад.** На вопрос в середине экрана (`/tp`, приглашение, вызов) отвечает крестовина: вправо как Y, влево как N.
- **Чит-коды.** Хост может запретить напарникам чит-коды галочкой в окне F9 (#118).
- **Предметы в анимациях.** При курении, питье, еде, разговоре по телефону и съёмке у CJ в руке сигарета, бутылка, банка, бургер, телефон или фотоаппарат, и другие игроки это видят. Когда вы пишете в чат, CJ коротко жестикулирует.
- **Гонки Сезара.** Гоняются все, кто стоит рядом с хостом на старте: у каждого такая же машина, свои чекпоинты и своё место (#139).
- **Втроём и вчетвером в одной машине.** Если у хоста нет свободного места, свою машину получает первый напарник, а остальные садятся к нему (#137). В «Reuniting the Families» у напарников своя машина за машиной Свита: первый ведёт её, остальные стреляют с неё и не ранят друг друга.
- **Пулемёты.** Очереди Hunter, Sea Sparrow и Rustler видны и слышны другим игрокам.
- **Совместимость.** Если игру запускает SA-MP или open.mp, мод выключается сам. С модом вида от первого лица (First Person Mod 3.0 от BoPoH) работают и свои скины.
- **Угол экрана.** Строки «SA:COOP · хост · пинг» в правом нижнем углу больше нет. Пинг каждого игрока виден в окне F2.

## Исправления

- Если сохранение хоста сделано с другим `data\script\main.scm`, окно F9 говорит об этом вместо вылета.
- Убраны вылеты при загрузке мира во время катсцены, на водителе случайной машины после окна F4, когда прохожий садится в удалённую машину, при закрытии игры со своими скинами, на анимациях людей (#116) и на испорченных командах скриптов (#120).
- После «Air Raid» у напарника больше не остаётся миниган.
- У хоста больше не звучит без конца миниган после того, как напарник постреляет из машины или катера.
- В миссиях Зеро самолётик и вертолётик напарника управляются как у хоста: газ на W, назад на S.
- Пока хост танцует в клубе, время у напарника больше не перематывается до полуночи и обратно.
- После покупки магазина Зеро, Wang Cars или аэродрома у напарника сразу пропадает значок продажи. Покупать ему не нужно, доход общий.
- Pay 'n' Spray снимает звёзды и с пассажира водителя (#135).
- Код приглашения больше не перестаёт работать через несколько минут после запуска сервера (#133, #138).
- Напарник может купить билет в аэропорту (#119).
- Дневной лимит в спортзале у напарника больше не берётся из мира хоста (#132).
- Девушку на свидании видит и другой игрок.
- Машина другого игрока в комнате тюнинга больше не видна и не мешает.
- Удар ножом со спины видят оба игрока.
- У напарника с модами карты больше не остаются барьеры на мостах.
- После загрузки мира хоста напарник выходит из Binco и других магазинов в своём городе, а не в Лос-Сантосе.
- В «Life's a Beach» и «555 We Tip» напарники выходят из закрытого гаража. Если хост едет пассажиром, а миссия ведёт машину сама, хост садится за руль.
- Если миссия переносит хоста вместе с машиной, машина напарника больше не появляется прямо в ней.

## Установка

Распакуйте `SA-COOP.zip` в папку игры, чтобы `coop.asi`, `install.bat` и папка `coop` оказались рядом с `gta_sa.exe`, и запустите `install.bat`. Подробная инструкция лежит в архиве в файле `Инструкция.txt`, а описание мода в README репозитория.

Чтобы обновиться с прошлой версии, распакуйте архив поверх старых файлов. Настройки и прогресс напарника останутся, а `install.bat` запускать второй раз не нужно. Файл `coop\packs\fortnite.ifp` от прошлой версии больше не нужен, и его можно удалить.

Версии 0.3.3 и 0.3.4 друг к другу не подключаются, поэтому обновиться нужно всем игрокам.

## In English

A mod for GTA San Andreas 1.0 US that lets two, three or four players go through the story together over the network.

### What's new

- **Skins, F4 key.** The skins window has the game's ordinary people by tabs, 266 skins from other authors' packs (new members of the Families, Ballas, Vagos and Aztecas, gangs of San Fierro and Las Venturas, bikers, beta versions of pedestrians and pedestrians cut from the game) and your own skins from the `coop\skins` folder. Everyone sees the skin you wear. It stays after loading a save and in cutscenes: the host plays CJ in their skin and moves its lips, and the partners stand next to them in theirs. In a skin your character walks like that person and talks with their voice.
- **The hero's voice in a skin.** In cutscenes and story dialogue the hero in a skin has a voice of their own, female or male by the skin, and CJ's name is heard neither in speech nor in subtitles. The voices are in the archive's `coop\voices` folder, so the archive is about 520 MB now. Without that folder CJ's voice plays.
- **Aboard without seats, G key.** In the Shamal and other planes with no passenger seats, and in helicopters and boats with no seat left, the G key takes you aboard with the driver. On a boat everyone has a seat of their own, and with the weapon view a player stands up and shoots. In a plane the first one sits next to the pilot. The camera change key switches the view.
- **Who the mission waits for.** In a mission's marker the host sees the nicknames of those it waits for, and the Y key starts the mission at once and brings the late ones over. If a partner stood in the marker first, Y takes the host right next to them.
- **Player colour.** The arrow colour a player picks in the F9 window or with `/blip` now also colours their nickname in the chat and over their head, their map waypoint and the route to it on the radar.
- **Gamepad.** The D-pad answers the question in the middle of the screen (`/tp`, an invitation, a call): right as Y, left as N.
- **Cheat codes.** The host can forbid cheat codes for the partners with a box in the F9 window (#118).
- **Props in animations.** When smoking, drinking, eating, on the phone or taking a picture CJ holds a cigarette, a bottle, a can, a burger, a phone or a camera, and the other players see it. When you write in the chat, CJ gestures briefly.
- **Cesar's races.** Everyone standing near the host at the start races: each gets the same car, their own checkpoints and their own place (#139).
- **Three or four in one car.** If the host has no free seat, the first partner gets a vehicle of their own and the others get in with them (#137). In Reuniting the Families the partners get their own car behind Sweet's: the first drives it, the others shoot from it and do not hurt each other.
- **Machine guns.** The bursts of the Hunter, Sea Sparrow and Rustler are seen and heard by the other players.
- **Compatibility.** If SA-MP or open.mp starts the game, the mod switches itself off. With the first person view mod (First Person Mod 3.0 by BoPoH) your own skins work too.
- **Screen corner.** The «SA:COOP · host · ping» line in the bottom right corner is gone. Every player's ping is in the F2 window.

### Fixes

- If the host's save was made with another `data\script\main.scm`, the F9 window says so instead of crashing.
- Crashes are gone when the world loads during a cutscene, on a random car's driver after the F4 window, when a pedestrian gets into a deleted car, when closing the game with your own skins, on people's animations (#116) and on broken script commands (#120).
- After Air Raid the partner no longer keeps the minigun.
- The host no longer hears an endless minigun after the partner shoots from a car or a boat.
- In Zero's missions the partner's RC plane and helicopter handle like the host's: throttle on W, back on S.
- While the host dances in the club, the partner's clock no longer jumps to midnight and back.
- After buying Zero's shop, Wang Cars or the airfield, the partner's for-sale icon goes at once. The partner does not need to buy it, the income is shared.
- Pay 'n' Spray clears the stars of the driver's passenger too (#135).
- The invite code no longer stops working a few minutes after the server starts (#133, #138).
- The partner can buy a plane ticket at the airport (#119).
- The partner's daily gym limit no longer comes from the host's world (#132).
- The other player sees the girlfriend on a date too.
- The other player's car in the mod shop's room is no longer visible and no longer gets in the way.
- Both players see a knife stealth kill.
- A partner with map mods no longer keeps the barriers on the bridges.
- After the host's world loads, the partner leaves Binco and other shops in their own city, not in Los Santos.
- In Life's a Beach and 555 We Tip the partners get out of the closed garage. If the host rides as a passenger while the mission drives the car, the host takes the wheel.
- If a mission moves the host together with their car, the partner's car no longer appears right inside it.

### Installation

Unpack `SA-COOP.zip` into the game folder so that `coop.asi`, `install.bat` and the `coop` folder sit next to `gta_sa.exe`, and run `install.bat`. The archive has detailed instructions in `Инструкция.txt` (Russian), and the repository's README describes the mod.

To update from the previous version, unpack the archive over the old files. The settings and the partner's progress stay, and `install.bat` does not need to run again. The `coop\packs\fortnite.ifp` file of the previous version is not needed any more and can be deleted.

Versions 0.3.3 and 0.3.4 cannot connect to each other, so all players need to update.
