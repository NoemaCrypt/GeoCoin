#import "GeoCoinTransactionPage.h"
#import "GeoCoinPageKit.h"

@interface GeoCoinTransactionPage () <NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate>
@property (nonatomic, weak) GeoCoinCoreBridge* core;
@property (nonatomic, strong) NSTableView* table;
@property (nonatomic, strong) NSTextField* search;
@property (nonatomic, strong) NSPopUpButton* type;
@property (nonatomic, copy) NSArray<NSDictionary<NSString*, NSString*>*>* allRows;
@property (nonatomic, copy) NSArray<NSDictionary<NSString*, NSString*>*>* rows;
@end

@implementation GeoCoinTransactionPage

- (NSString*)localized:(NSString*)text { return [self.core localized:text context:@"TransactionPage"]; }

- (instancetype)initWithCore:(GeoCoinCoreBridge*)core
{
    if ((self = [super initWithFrame:NSZeroRect])) {
        _core = core;
        self.identifier = @"history";
        _rows = @[];
        NSStackView* body;
        NSView* page = GeoMakePage([self localized:@"Transaction History"], &body);
        [self addSubview:page];
        [NSLayoutConstraint activateConstraints:@[
            [page.topAnchor constraintEqualToAnchor:self.topAnchor],
            [page.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [page.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [page.trailingAnchor constraintEqualToAnchor:self.trailingAnchor]
        ]];

        NSStackView* filters = GeoMakeRow(8);
        _search = GeoMakeField([self localized:@"Search by address, txid, amount..."]);
        _search.delegate = self;
        [_search setAccessibilityLabel:[self localized:@"Search transactions"]];
        _type = [NSPopUpButton new];
        [_type setAccessibilityLabel:[self localized:@"Transaction type"]];
        for (NSString* type in @[@"All Types", @"Received", @"Sent", @"Self", @"Mined"]) {
            [_type addItemWithTitle:[self localized:type]];
        }
        _type.target = self;
        _type.action = @selector(applyFilters);
        [filters addArrangedSubview:_search];
        [filters addArrangedSubview:_type];
        [body addArrangedSubview:filters];

        NSScrollView* scroll = [NSScrollView new];
        scroll.hasVerticalScroller = YES;
        scroll.hasHorizontalScroller = YES;
        scroll.translatesAutoresizingMaskIntoConstraints = NO;
        [scroll.heightAnchor constraintGreaterThanOrEqualToConstant:220].active = YES;
        _table = [NSTableView new];
        _table.usesAlternatingRowBackgroundColors = YES;
        _table.dataSource = self;
        _table.delegate = self;
        _table.target = self;
        _table.doubleAction = @selector(openSelectedTransaction);
        [self setColumnHeadings];
        scroll.documentView = _table;
        [body addArrangedSubview:scroll];

        __weak GeoCoinTransactionPage* weakPage = self;
        core.onTransactionsReset = ^{ [weakPage reload]; };
        [self reload];
    }
    return self;
}

- (void)setColumnHeadings
{
    NSArray<NSString*>* names = @[@"Status", @"Date", @"Type", @"Address", @"Amount"];
    const CGFloat widths[] = {78, 146, 105, 260, 135};
    for (NSUInteger index = 0; index < names.count; ++index) {
        NSString* key = [NSString stringWithFormat:@"col%lu", (unsigned long)index];
        NSTableColumn* column = [self.table tableColumnWithIdentifier:key];
        if (!column) {
            column = [[NSTableColumn alloc] initWithIdentifier:key];
            column.width = widths[index];
            column.minWidth = 55;
            [self.table addTableColumn:column];
        }
        column.title = [self.core localized:names[index] context:@"TransactionTableModel"];
    }
}

- (void)reload
{
    self.allRows = [self.core transactions];
    [self applyFilters];
}

- (void)applyFilters
{
    NSString* search = [self.search.stringValue stringByTrimmingCharactersInSet:
        NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSInteger type = self.type.indexOfSelectedItem - 1;
    NSMutableArray* rows = [NSMutableArray array];
    for (NSDictionary* row in self.allRows) {
        if (type >= 0 && [row[@"type"] integerValue] != type) continue;
        if (search.length &&
            ![row[@"txid"] localizedCaseInsensitiveContainsString:search] &&
            ![row[@"address"] localizedCaseInsensitiveContainsString:search] &&
            ![row[@"col4"] localizedCaseInsensitiveContainsString:search]) continue;
        [rows addObject:row];
    }
    self.rows = rows;
    [self.table reloadData];
}

- (void)controlTextDidChange:(NSNotification*)notification
{
    (void)notification;
    [self applyFilters];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    (void)tableView;
    return (NSInteger)self.rows.count;
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row
{
    NSTextField* cell = [tableView makeViewWithIdentifier:column.identifier owner:self];
    if (!cell) {
        cell = [NSTextField labelWithString:@""];
        cell.identifier = column.identifier;
        cell.lineBreakMode = NSLineBreakByTruncatingMiddle;
    }
    cell.stringValue = self.rows[(NSUInteger)row][column.identifier] ?: @"";
    if ([column.identifier isEqualToString:@"col4"]) cell.alignment = NSTextAlignmentRight;
    return cell;
}

- (void)openSelectedTransaction
{
    NSInteger selected = self.table.clickedRow;
    if (selected < 0 || selected >= (NSInteger)self.rows.count) return;
    NSDictionary* row = self.rows[(NSUInteger)selected];
    NSAlert* alert = [NSAlert new];
    alert.messageText = [self localized:@"Transaction Details"];
    alert.informativeText = [NSString stringWithFormat:@"%@   %@\n%@\n%@\n\n%@",
        row[@"col2"], row[@"col4"], row[@"col1"], row[@"address"], row[@"txid"]];
    [alert addButtonWithTitle:[self localized:@"Close"]];
    [alert addButtonWithTitle:[self localized:@"Copy Transaction ID"]];
    [alert beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        if (response != NSAlertSecondButtonReturn) return;
        [[NSPasteboard generalPasteboard] clearContents];
        [[NSPasteboard generalPasteboard] setString:row[@"txid"] ?: @"" forType:NSPasteboardTypeString];
    }];
}

- (void)applyTheme {}
- (void)applyLanguage { [self setColumnHeadings]; [self reload]; }

@end
