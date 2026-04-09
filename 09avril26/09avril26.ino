#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "LittleFS.h"
#include <vector>
#include <map>

// ===== BIBLIOTHÈQUES BLUETOOTH =====
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ===== CONFIGURATION CIVVI =====
String monNomReseau = "Civvi-C3-SuperMini"; // Nom qui apparaitra en Bluetooth !
String maPensee = "En attente d'inspiration...";
String monID = "";
uint32_t monMsgID = 0; // Identifiant unique pour le texte découpé

#define MAX_MESSAGES 15
#define TIMEOUT_MS 180000 // 3 minutes avant disparition radar
#define MAX_PAYLOAD_SIZE 180 // Taille max d'un morceau de texte par ondes
#define MAX_CHUNKS 35 // 35 morceaux * 180 = 6300 caractères

uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
esp_now_peer_info_t peerInfo;

// ===== STRUCTURES =====
struct Message {
  String auteur;
  String nomReseau;
  String texte;
  int rssi;
  unsigned long dernierContact;
};

struct MessageIncomplet {
  String nomReseau;
  int total;
  int recu;
  int rssi;
  String morceaux[MAX_CHUNKS];
  unsigned long dernierUpdate;
};

std::vector<Message> messages;
std::map<String, MessageIncomplet> messagesEnAttente;

unsigned long dernierEnvoi = 0;
volatile bool resauvegarder = false;

// ===== BLUETOOTH UART (Nordic Service) =====
#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E" 
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLEServer *pServer = NULL;
BLECharacteristic * pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };
    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

void envoyerJsonBLE(); // Déclaration préalable

String bufferBLE = "";
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      size_t len = pCharacteristic->getLength();
      uint8_t* data = pCharacteristic->getData();
      String chunk = "";
      for (int i = 0; i < len; i++) chunk += (char)data[i];
      
      bufferBLE += chunk;
      
      // La page web ajoute \n à la fin de son envoi complet
      if(bufferBLE.endsWith("\n")) {
         bufferBLE.trim(); // Enlève le \n
         if(bufferBLE.length() > 0) {
            maPensee = bufferBLE;
            // On sauvegarde l'état
            File f = LittleFS.open("/pensees.txt", FILE_WRITE);
            if(f) { f.println(monID + "|" + monNomReseau + "|" + maPensee); f.close(); }
            
            // On annonce à tous les autres ESP32 
            void envoyerTexteLong();
            envoyerTexteLong();
            
            // On rafraîchit l'interface BLE
            envoyerJsonBLE(); 
         }
         bufferBLE = "";
      }
    }
};

void envoyerJsonBLE() {
  if(!deviceConnected) return;

  String json = "[";
  json += "{\"auteur\":\"" + monID + "\",\"nomReseau\":\"" + monNomReseau + "\",\"texte\":\"" + maPensee + "\",\"rssi\":0}";
  for (int i = 0; i < messages.size(); i++) {
    json += ",{\"auteur\":\"" + messages[i].auteur + "\",\"nomReseau\":\"" + messages[i].nomReseau + "\",\"texte\":\"" + messages[i].texte + "\",\"rssi\":" + String(messages[i].rssi) + "}";
  }
  json += "]\n"; // Le marqueur de fin pour la page web
  
  // Pour éviter la saturation du tampon Bluetooth (Maximum Transmission Unit), on compresse par petites briques.
  for(int i = 0; i < json.length(); i += 80) {
    String chunk = json.substring(i, i + 80);
    pTxCharacteristic->setValue((uint8_t*)chunk.c_str(), chunk.length());
    pTxCharacteristic->notify();
    delay(20); // Petite respiration matérielle
  }
}

// ===== LE DÉCOUPEUR INTELLIGENT (Envoi ESP-NOW) =====
void envoyerTexteLong() {
  String texteRestant = maPensee;
  std::vector<String> morceaux;

  while(texteRestant.length() > 0) {
    if(texteRestant.length() <= MAX_PAYLOAD_SIZE) {
      morceaux.push_back(texteRestant);
      break;
    }
    int splitPos = MAX_PAYLOAD_SIZE;
    for(int i = MAX_PAYLOAD_SIZE; i > MAX_PAYLOAD_SIZE - 40; i--) {
      char c = texteRestant[i];
      if(c == ' ' || c == '.' || c == ',' || c == '!') { splitPos = i + 1; break; }
    }
    morceaux.push_back(texteRestant.substring(0, splitPos));
    texteRestant = texteRestant.substring(splitPos);
  }

  int total = morceaux.size();
  if (total > MAX_CHUNKS) total = MAX_CHUNKS; 
  monMsgID = millis(); 
  
  for(int i = 0; i < total; i++) {
    String paquet = monID + "|" + monNomReseau + "|" + String(monMsgID) + "|" + String(total) + "|" + String(i) + "|" + morceaux[i];
    esp_now_send(broadcastAddress, (uint8_t*)paquet.c_str(), paquet.length());
    delay(40); 
  }
}

// ===== LE RÉASSEMBLEUR (Réception ESP-NOW) =====
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  char temp[len+1];
  memcpy(temp, data, len);
  temp[len] = '\0';
  String msg = String(temp);

  int s[5];
  s[0] = msg.indexOf('|');
  for(int i=1; i<5; i++) s[i] = msg.indexOf('|', s[i-1] + 1);
  
  if (s[4] > 0) {
    String auteur = msg.substring(0, s[0]);
    String nom = msg.substring(s[0]+1, s[1]);
    String msgId = msg.substring(s[1]+1, s[2]);
    int total = msg.substring(s[2]+1, s[3]).toInt();
    int index = msg.substring(s[3]+1, s[4]).toInt();
    String texteChunk = msg.substring(s[4]+1);
    int rssi = info->rx_ctrl->rssi;

    String cleUnique = auteur + "_" + msgId;

    if (index >= MAX_CHUNKS || index < 0) return;

    if (messagesEnAttente.find(cleUnique) == messagesEnAttente.end()) {
      messagesEnAttente[cleUnique] = {nom, total, 0, rssi, {""}, millis()};
    }

    if (messagesEnAttente[cleUnique].morceaux[index] == "") {
      messagesEnAttente[cleUnique].morceaux[index] = texteChunk;
      messagesEnAttente[cleUnique].recu++;
      messagesEnAttente[cleUnique].rssi = rssi;
    }

    if (messagesEnAttente[cleUnique].recu == total) {
      String texteComplet = "";
      for(int i=0; i<total; i++) texteComplet += messagesEnAttente[cleUnique].morceaux[i];
      
      bool found = false;
      for (auto &m : messages) {
        if (m.auteur == auteur) {
          m.nomReseau = nom;
          m.texte = texteComplet;
          m.rssi = rssi;
          m.dernierContact = millis();
          found = true; break;
        }
      }
      if (!found) {
        messages.push_back({auteur, nom, texteComplet, rssi, millis()});
        if (messages.size() > MAX_MESSAGES) messages.erase(messages.begin());
      }
      messagesEnAttente.erase(cleUnique);
      resauvegarder = true; 
    }
  }
}

// ===== INITIALISATION =====
void setup() {
  Serial.begin(115200);
  Serial.println("Initialisation Civvi Bluetooth...");

  // MODE STATION POUR LIBÉRER LE WIFI, sans connexion à une box (pour ESP-NOW uniquement)
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  if (esp_wifi_set_promiscuous(true) != ESP_OK) {
    Serial.println("Erreur mode promiscuous");
  }
  if (esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    Serial.println("Erreur configuration canal ESP-NOW");
  }
  esp_wifi_set_promiscuous(false);

  monID = WiFi.macAddress();
  monID.replace(":", "");

  if (LittleFS.begin(true)) {
    if (LittleFS.exists("/pensees.txt")) {
      File f = LittleFS.open("/pensees.txt", FILE_READ);
      if (f.available()) {
        String ligne = f.readStringUntil('\n');
        ligne.trim();
        int s1 = ligne.indexOf('|');
        int s2 = ligne.indexOf('|', s1 + 1);
        if (s1 > 0 && s2 > 0) {
          maPensee = ligne.substring(s2 + 1);
        }
      }
      f.close();
    }
  }
  
  if (esp_now_init() != ESP_OK) return;
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 1;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);
  esp_now_register_recv_cb(onReceive);

  // CONFIGURATION DU BLUETOOTH BLE
  BLEDevice::init(monNomReseau.c_str()); // Nom visible dans le scanner Bluetooth
  
  // On ne limite pas la puissance du S3, les réglages natifs vont bien.
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID_TX,
                        BLECharacteristic::PROPERTY_NOTIFY
                      );
  pTxCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(
                                         CHARACTERISTIC_UUID_RX,
                                         BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
                                       );
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pService->start();
  
  // Mettons ça en public pour un appairage facile
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Aide le tel a trouver l'appareil vite
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("Serveur BLE prêt ! En attente d'un téléphone...");
}

// ===== BOUCLE PRINCIPALE =====
void loop() {
  
  // On a activé la sauvegarde dans `onReceive`
  if (resauvegarder) {
    envoyerJsonBLE(); // On prévient immédiatement le téléphone si branché !
    resauvegarder = false;
  }

  // Nettoyage radar et des morceaux cassés
  bool modif = false;
  for (int i = 0; i < messages.size(); i++) {
    if (millis() - messages[i].dernierContact > TIMEOUT_MS) {
      messages.erase(messages.begin() + i);
      i--; 
      modif = true;
    }
  }
  if (modif) envoyerJsonBLE(); // Le radar a effacé quelqu'un, on rafraichit l'écran !

  for (auto it = messagesEnAttente.begin(); it != messagesEnAttente.end(); ) {
    if (millis() - it->second.dernierUpdate > 10000) { 
      it = messagesEnAttente.erase(it);
    } else {
      ++it;
    }
  }

  // Auto-diffusion ESP-NOW (Chaque 10s)
  if (millis() - dernierEnvoi > 10000) {
    envoyerTexteLong();
    dernierEnvoi = millis();
  }

  // GESTION BLUETOOTH CONNEXION
  if (deviceConnected && !oldDeviceConnected) {
      delay(500); // Laisse le téléphone configurer ses données
      envoyerJsonBLE(); 
      oldDeviceConnected = deviceConnected;
      Serial.println("Téléphone connecté !");
  }
  if (!deviceConnected && oldDeviceConnected) {
      delay(500); 
      pServer->startAdvertising(); 
      Serial.println("Téléphone déconnecté. Remise en publicité bluetooth.");
      oldDeviceConnected = deviceConnected;
  }
}
