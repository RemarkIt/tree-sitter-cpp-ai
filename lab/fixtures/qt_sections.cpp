#define signals public
#define slots
#define Q_SIGNALS public
#define Q_SLOTS
#define PRIVATE_UNLESS_TESTED private
class Session {
public:
    int value() { return 1; }
signals:
    void changed(int value);
public slots:
    void refresh() {}
private Q_SLOTS: // timer callbacks
    void onTimeout() {}
Q_SIGNALS:
    void finished();
PRIVATE_UNLESS_TESTED:
    int count() { return 2; }
};
