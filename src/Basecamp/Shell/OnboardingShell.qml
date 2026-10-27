import QtQuick

import Logos.Theme
import Logos.Icons
import Basecamp.Icons

// Root of the first-run onboarding layer, which covers the window until the
// user has chosen how apps download. The only onboarding file that reaches the
// backend: the views below emit signals.
Rectangle {
    id: root

    signal finished()

    color: Theme.palette.background

    OnboardingView {
        objectName: "onboardingView"
        anchors.fill: parent
        backdrop: LogosIcons.onboardingBackdrop
        logo: BasecampIcons.logo
        storageState: backend.storageState
        storageError: backend.storageError
        onSourceChosen: (source) => backend.chooseOnboardingSource(source)
        onRetryStorageRequested: backend.retryStorage()
        onFinished: {
            backend.completeOnboarding()
            root.finished()
        }
    }
}
