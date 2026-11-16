// SPDX-License-Identifier: MIT
// 20H392 OSLogStore accepts only process scope through its public factory.
// Reuse its native local-store preparation and original unconstrained enumerator.
#ifndef QCOM_IOS16_LOG_STORE_H
#define QCOM_IOS16_LOG_STORE_H
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <string.h>

@protocol QcomIos16LogStoreInit <NSObject>
- (instancetype)initForFactory;
@end

namespace QcomUserLog {
static id storeFailure(NSError **error, NSString *reason)
{
    if (error) *error = [NSError errorWithDomain:@"org.unixnu.qcom.logs" code:1
                                      userInfo:@{NSLocalizedDescriptionKey:reason}];
    return nil;
}

static id openIos16SystemStore(Class storeClass, NSError **error,
                              int64_t timeoutNs = 30 * (int64_t)NSEC_PER_SEC)
{
    if (error) *error = nil;
    Ivar sourceIvar = storeClass ? class_getInstanceVariable(storeClass, "_source") : nullptr;
    Ivar constraintIvar = storeClass ? class_getInstanceVariable(storeClass, "_constraint") : nullptr;
    const char *sourceType = sourceIvar ? ivar_getTypeEncoding(sourceIvar) : nullptr;
    const char *constraintType = constraintIvar ? ivar_getTypeEncoding(constraintIvar) : nullptr;
    // The original class_ro extent is 20 bytes; a runtime may report its
    // pointer-aligned allocation extent (24). Both describe these same fields.
    const size_t extent = storeClass ? class_getInstanceSize(storeClass) : 0;
    if (!storeClass || (extent != 20 && extent != 24) ||
        !sourceType || sourceType[0] != '@' || ivar_getOffset(sourceIvar) != 8 ||
        !constraintType || strcmp(constraintType, "i") || ivar_getOffset(constraintIvar) != 16 ||
        !class_respondsToSelector(storeClass, @selector(initForFactory)) ||
        timeoutNs < 0 || timeoutNs > 30 * (int64_t)NSEC_PER_SEC)
        return storeFailure(error, [NSString stringWithFormat:
            @"20H392 OSLogStore ABI mismatch size=%zu source=%ld:%s constraint=%ld:%s",
            extent, sourceIvar ? (long)ivar_getOffset(sourceIvar) : -1L, sourceType ?: "?",
            constraintIvar ? (long)ivar_getOffset(constraintIvar) : -1L, constraintType ?: "?"]);

    Class factoryClass = NSClassFromString(@"OSLogEventStore");
    Class sourceClass = NSClassFromString(@"OSLogEventSource");
    if (!factoryClass || !sourceClass || ![factoryClass respondsToSelector:@selector(localStore)])
        return storeFailure(error, @"LoggingSupport local store unavailable");
    id local = ((id (*)(Class, SEL))objc_msgSend)(factoryClass, @selector(localStore));
    if (!local || ![local respondsToSelector:@selector(prepareWithCompletionHandler:)])
        return storeFailure(error, @"LoggingSupport local store preparation unavailable");

    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    __block id prepared = nil;
    __block NSError *failure = nil;
    // Capture owned objects, never the caller's NSError **. A late native
    // completion remains safe after a bounded wait times out.
    void (^completion)(id, NSError *) = ^(id source, NSError *nativeError) {
        prepared = source; failure = nativeError;
        dispatch_semaphore_signal(ready);
    };
    ((id (*)(id, SEL, id))objc_msgSend)(local, @selector(prepareWithCompletionHandler:), completion);
    if (dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, timeoutNs)))
        return storeFailure(error, @"LoggingSupport local store preparation timeout");
    if (failure) { if (error) *error = failure; return nil; }
    if (!prepared || ![prepared isKindOfClass:sourceClass])
        return storeFailure(error, @"LoggingSupport returned no valid event source");

    id store = [(id<QcomIos16LogStoreInit>)[storeClass alloc] initForFactory];
    if (!store || object_getClass(store) != storeClass)
        return storeFailure(error, @"OSLogStore initializer returned no exact store");
    int constraint = -1;
    memcpy(&constraint, (const char *)(__bridge const void *)store + ivar_getOffset(constraintIvar), sizeof constraint);
    if (constraint != 0 || object_getIvar(store, sourceIvar))
        return storeFailure(error, @"OSLogStore initializer has unexpected state");
    // Match the original storeWithURL: completion's strong _source assignment.
    // The zero constraint is the original enumerator's system-source path.
    object_setIvarWithStrongDefault(store, sourceIvar, prepared);
    if (object_getIvar(store, sourceIvar) != prepared)
        return storeFailure(error, @"OSLogStore source assignment failed");
    return store;
}
}
#endif
