import SwiftUI
import WidgetKit

// The widget extension exists only to draw the transfer Live Activity; it has
// no Home Screen widgets.
@main
struct TransferWidgetBundle: WidgetBundle {
    var body: some Widget {
        TransferLiveActivity()
    }
}
