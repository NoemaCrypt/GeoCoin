#import "GeoCoinPageKit.h"
#import <QuartzCore/QuartzCore.h>

@interface GeoThemeLabel : NSTextField
@property (nonatomic, copy) NSString* colorRole;
@end

@implementation GeoThemeLabel
@end

GeoStack* GeoMakeStack(CGFloat spacing)
{
    GeoStack* stack = [NSStackView new];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = spacing;
    stack.distribution = NSStackViewDistributionFill;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    return stack;
}

GeoStack* GeoMakeRow(CGFloat spacing)
{
    GeoStack* row = GeoMakeStack(spacing);
    row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    row.alignment = NSLayoutAttributeCenterY;
    return row;
}

NSTextField* GeoMakeLabel(NSString* text, NSColor* color, CGFloat size)
{
    GeoThemeLabel* label = [[GeoThemeLabel alloc] initWithFrame:NSZeroRect];
    GeoGlassColors* palette = [GeoThemeManager shared].colors;
    if (!color || [color isEqual:palette.text]) label.colorRole = @"text";
    else if ([color isEqual:palette.mutedText]) label.colorRole = @"mutedText";
    label.stringValue = text ?: @"";
    label.editable = NO;
    label.selectable = NO;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.font = [NSFont systemFontOfSize:(size > 0 ? size : 13) weight:NSFontWeightSemibold];
    label.textColor = color ?: palette.text;
    label.translatesAutoresizingMaskIntoConstraints = NO;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}

NSTextField* GeoMakeField(NSString* placeholder)
{
    GeoField* field = [[GeoField alloc] initWithFrame:NSZeroRect];
    field.placeholderString = placeholder;
    field.translatesAutoresizingMaskIntoConstraints = NO;
    field.editable = YES;
    field.selectable = YES;
    field.font = [NSFont systemFontOfSize:13 weight:NSFontWeightRegular];
    field.focusRingType = NSFocusRingTypeExterior;
    [field setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
                                     forOrientation:NSLayoutConstraintOrientationHorizontal];
    [field.heightAnchor constraintGreaterThanOrEqualToConstant:27].active = YES;
    [field applyTheme];
    return field;
}

GeoGlassPanel* GeoMakeGroup(NSString* title, NSStackView** outBody)
{
    GeoGlassPanel* panel = [[GeoGlassPanel alloc] initWithFrame:NSZeroRect];
    panel.translatesAutoresizingMaskIntoConstraints = NO;
    panel.sectionTitle = title;

    NSStackView* body = GeoMakeStack(5);
    [panel addSubview:body];
    [NSLayoutConstraint activateConstraints:@[
        [body.leadingAnchor constraintEqualToAnchor:panel.leadingAnchor constant:17],
        [body.trailingAnchor constraintEqualToAnchor:panel.trailingAnchor constant:-17],
        [body.topAnchor constraintEqualToAnchor:panel.topAnchor constant:(title.length ? 43 : 14)],
        [body.bottomAnchor constraintEqualToAnchor:panel.bottomAnchor constant:-14],
    ]];
    if (outBody) *outBody = body;
    return panel;
}

GeoPillButton* GeoMakePill(NSString* title, void (^action)(void))
{
    GeoPillButton* button = [[GeoPillButton alloc] initWithFrame:NSZeroRect];
    button.title = title ?: @"";
    button.geoAction = action;
    return button;
}

NSView* GeoMakePage(NSString* title, NSStackView** outBody)
{
    NSStackView* root = GeoMakeStack(12);
    NSTextField* titleLabel = GeoMakeLabel(title, nil, 20);
    titleLabel.identifier = @"geo-page-title";
    titleLabel.font = [NSFont systemFontOfSize:20 weight:NSFontWeightBold];
    [root addArrangedSubview:titleLabel];

    NSStackView* body = GeoMakeStack(14);
    [root addArrangedSubview:body];
    GeoFill(body);
    if (outBody) *outBody = body;
    return root;
}

void GeoFill(NSView* view)
{
    view.translatesAutoresizingMaskIntoConstraints = NO;
    NSView* parent = view.superview;
    if ([parent isKindOfClass:NSStackView.class]) {
        NSStackView* stack = (NSStackView*)parent;
        if (stack.orientation == NSUserInterfaceLayoutOrientationVertical) {
            [view.widthAnchor constraintEqualToAnchor:stack.widthAnchor
                    constant:-(stack.edgeInsets.left + stack.edgeInsets.right)].active = YES;
        }
    }
}

void GeoApplyThemeToView(NSView* view)
{
    if ([view isKindOfClass:GeoThemeLabel.class]) {
        GeoThemeLabel* label = (GeoThemeLabel*)view;
        if (label.colorRole) label.textColor = GeoThemeColor(label.colorRole);
    } else if ([view isKindOfClass:GeoField.class]) {
        [(GeoField*)view applyTheme];
    } else if ([view isKindOfClass:GeoGlassPanel.class]) {
        [(GeoGlassPanel*)view applyTheme];
    } else if ([view isKindOfClass:GeoPillButton.class]) {
        [(GeoPillButton*)view applyTheme];
    }
    for (NSView* child in view.subviews) GeoApplyThemeToView(child);
}
