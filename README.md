ESP32 BTC/USDT Ticker (Binance)

ESP32 + ILI9488 TFT ekran ile gerçek zamanlı **BTC/USDT fiyat takibi**.

Proje, **Binance public API** üzerinden son fiyat (`lastPrice`) ve 24 saatlik değişim yüzdesi (`priceChangePercent`) bilgilerini alır.  
Ekranda büyük fiyat yazısı, renkli 24h değişim oku (yeşil ↑ / kırmızı ↓) ve son güncelleme saati gösterilir.  

✨ Özellikler
- ESP32 + ILI9488 (TFT_eSPI kütüphanesi)
- Binance **/api/v3/ticker/24hr** uç noktası (API key gerekmez)
- Her **15 saniyede bir güncelleme**
- **24h değişim yüzdesi** renkli gösterim
- WiFi yeniden bağlanma + API hatalarında **üstel backoff**
- **NTP saat** gösterimi (GMT+3 ayarlı)
- Tüm çizimler **sprite** üzerinden → flicker yok
  
 🔧 Gereksinimler
- ESP32 kart
- ILI9488 TFT ekran
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) kütüphanesi  
  - `User_Setup.h` içinde `#define ILI9488_DRIVER`
- [ArduinoJson](https://arduinojson.org/) kütüphanesi
- WiFi erişimi
