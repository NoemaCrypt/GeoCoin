#ifndef GEOCOIN_MACOS_NATIVE_FRONTEND_H
#define GEOCOIN_MACOS_NATIVE_FRONTEND_H

#include <memory>

class ClientModel;
class GeoCoinApplication;
class GeoCoinGUI;
class QString;
class WalletModel;

class GeoCoinNativeFrontend {
public:
    GeoCoinNativeFrontend(GeoCoinApplication& app, GeoCoinGUI& workflows,
                          const QString& dataDirectory, const QString& network);
    ~GeoCoinNativeFrontend();

    void setModels(ClientModel* client, WalletModel* wallet);
    void setSyncProgress(double fraction, int current, int target);
    void beginShutdown();
    bool runSmokeChecks(bool layout, QString* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

QString geoCoinPickNativeDataDirectory();

#endif
