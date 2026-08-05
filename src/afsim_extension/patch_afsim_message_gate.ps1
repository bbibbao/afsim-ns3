param(
    [Parameter(Mandatory = $true)]
    [string]$AfsimSourceRoot
)

$ErrorActionPreference = "Stop"

$sourceRoot = (Resolve-Path $AfsimSourceRoot).Path
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$backupRoot = Join-Path $projectRoot (
    ".codex-backups\afsim-message-gate-" +
    (Get-Date -Format "yyyyMMdd-HHmmss")
)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Update-SourceFile {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [array]$Changes
    )

    if (-not (Test-Path $Path -PathType Leaf)) {
        throw "AFSIM source file not found: $Path"
    }

    $original = [System.IO.File]::ReadAllText($Path)
    $usesCrLf = $original.Contains("`r`n")
    $updated = $original.Replace("`r`n", "`n")

    foreach ($change in $Changes) {
        $newText = ([string]$change.New).Replace("`r`n", "`n")
        if ($updated.Contains($newText)) {
            continue
        }

        $oldOptions = if ($change.ContainsKey("OldOptions")) {
            @($change.OldOptions)
        }
        else {
            @($change.Old)
        }
        $selectedOld = $null
        foreach ($oldOption in $oldOptions) {
            $oldText = ([string]$oldOption).Replace("`r`n", "`n")
            $count = [regex]::Matches(
                $updated,
                [regex]::Escape($oldText)
            ).Count
            if ($count -gt 1) {
                throw "Expected at most one patch location in $Path, found $count"
            }
            if ($count -eq 1) {
                if ($null -ne $selectedOld) {
                    throw "Multiple alternative patch locations found in $Path"
                }
                $selectedOld = $oldText
            }
        }
        if ($null -eq $selectedOld) {
            throw "No supported patch location found in $Path"
        }
        $updated = $updated.Replace($selectedOld, $newText)
    }

    $normalizedOriginal = $original.Replace("`r`n", "`n")
    if ($updated -eq $normalizedOriginal) {
        Write-Output "[exists] $Path"
        return
    }

    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    Copy-Item $Path (Join-Path $backupRoot ([System.IO.Path]::GetFileName($Path)))
    if ($usesCrLf) {
        $updated = $updated.Replace("`n", "`r`n")
    }
    [System.IO.File]::WriteAllText($Path, $updated, $utf8NoBom)
    Write-Output "[patched] $Path"
}

function Remove-SourceText {
    param(
        [Parameter(Mandatory = $true)] [string]$Path,
        [Parameter(Mandatory = $true)] [array]$Texts
    )

    if (-not (Test-Path $Path -PathType Leaf)) {
        throw "AFSIM source file not found: $Path"
    }

    $original = [System.IO.File]::ReadAllText($Path)
    $usesCrLf = $original.Contains("`r`n")
    $updated = $original.Replace("`r`n", "`n")
    foreach ($text in $Texts) {
        $normalized = ([string]$text).Replace("`r`n", "`n")
        $updated = $updated.Replace($normalized, "")
    }
    if ($updated -eq $original.Replace("`r`n", "`n")) {
        Write-Output "[clean] $Path"
        return
    }

    New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
    Copy-Item $Path (Join-Path $backupRoot ([System.IO.Path]::GetFileName($Path))) -Force
    if ($usesCrLf) {
        $updated = $updated.Replace("`n", "`r`n")
    }
    [System.IO.File]::WriteAllText($Path, $updated, $utf8NoBom)
    Write-Output "[cleaned] $Path"
}

$commRoot = Join-Path $sourceRoot "core\wsf\source\comm"
$resultHeader = Join-Path $commRoot "WsfCommResult.hpp"
$guidedSource = Join-Path $commRoot "WsfCommMediumGuided.cpp"
$unguidedSource = Join-Path $commRoot "WsfCommMediumUnguided.cpp"

$resultMacro = @'
#define WSF_COMM_EXTERNAL_NETWORK_GATE 1
'@
$resultFields = @'
   //! True when an external network model supplied the delivery decision.
   bool mExternalNetworkGateApplied{false};

   //! True when the externally modeled message must not reach the receiver.
   bool mExternalNetworkDrop{false};

   //! Additional delivery delay supplied by the external model, in seconds.
   double mExternalNetworkDelaySeconds{0.0};
'@
Remove-SourceText $resultHeader @($resultMacro, $resultFields)

$transmittedOld = @'
   WsfObserver::MessageTransmitted(GetSimulation())(aSimTime, &aXmtr, *(aMessage.SourceMessage()));
'@
$transmittedNew = @'
   // MessageTransmitted is emitted after the external network gate below.
'@

$guidedIncludeOld = @'
#include "WsfCommNetworkManager.hpp"
'@
$guidedIncludeNew = @'
#include "WsfCommNetworkManager.hpp"
#include "WsfCommObserver.hpp"
'@
$guidedStringIncludeOld = @'
#include "WsfCommPhysicalLayer.hpp"
'@
$guidedStringIncludeNew = @'
#include "WsfCommPhysicalLayer.hpp"

#include <string>
'@
$guidedStatusOld = @'
   status.SetTimeLastXmtrStatusChange(aXmtr.GetLastStatusChangeTime());

   // Determine if the recipient is actually able to receive a message based on simulation
'@
$guidedStatusPrevious = @'
   status.SetTimeLastXmtrStatusChange(aXmtr.GetLastStatusChangeTime());

   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   if (externalResult.mExternalNetworkGateApplied)
   {
      if (externalResult.mExternalNetworkDrop)
      {
         status.SetAbortDelivery(true);
      }
      else if (externalResult.mExternalNetworkDelaySeconds > 0.0)
      {
         deliveryTime += externalResult.mExternalNetworkDelaySeconds;
         status.SetTimeDelivery(deliveryTime);
      }
   }
   if (!externalResult.mExternalNetworkGateApplied || !externalResult.mExternalNetworkDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }

   // Determine if the recipient is actually able to receive a message based on simulation
'@
$guidedStatusCurrent = @'
   status.SetTimeLastXmtrStatusChange(aXmtr.GetLastStatusChangeTime());

   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   const std::string gateDropPrefix{"AFSIM_NS3_GATE_DROP:"};
   const std::string gateDelayPrefix{"AFSIM_NS3_GATE_DELAY_SECONDS:"};
   const bool externalDrop =
      externalResult.mDetailedReason.compare(
         0, gateDropPrefix.size(), gateDropPrefix) == 0;
   double externalDelaySeconds = 0.0;
   if (externalResult.mDetailedReason.compare(
          0, gateDelayPrefix.size(), gateDelayPrefix) == 0)
   {
      try
      {
         externalDelaySeconds = std::stod(
            externalResult.mDetailedReason.substr(gateDelayPrefix.size()));
      }
      catch (...)
      {
         externalDelaySeconds = 0.0;
      }
   }
   if (externalDrop)
   {
      status.SetAbortDelivery(true);
   }
   else if (externalDelaySeconds > 0.0)
   {
      deliveryTime += externalDelaySeconds;
      status.SetTimeDelivery(deliveryTime);
   }
   if (!externalDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }

   // Determine if the recipient is actually able to receive a message based on simulation
'@
$guidedStatusFloor = @'
   status.SetTimeLastXmtrStatusChange(aXmtr.GetLastStatusChangeTime());

   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   const std::string gateDropPrefix{"AFSIM_NS3_GATE_DROP:"};
   const std::string gateTotalDelayPrefix{"AFSIM_NS3_GATE_TOTAL_DELAY_SECONDS:"};
   const bool externalDrop =
      externalResult.mDetailedReason.compare(
         0, gateDropPrefix.size(), gateDropPrefix) == 0;
   bool externalTotalDelaySpecified = false;
   double externalTotalDelaySeconds = 0.0;
   if (externalResult.mDetailedReason.compare(
          0, gateTotalDelayPrefix.size(), gateTotalDelayPrefix) == 0)
   {
      try
      {
         externalTotalDelaySeconds = std::stod(
            externalResult.mDetailedReason.substr(gateTotalDelayPrefix.size()));
         if (externalTotalDelaySeconds < 0.0)
         {
            externalTotalDelaySeconds = 0.0;
         }
         externalTotalDelaySpecified = true;
      }
      catch (...)
      {
         externalTotalDelaySeconds = 0.0;
      }
   }
   if (externalDrop)
   {
      status.SetAbortDelivery(true);
   }
   else if (externalTotalDelaySpecified)
   {
      const double requestedDeliveryTime =
         aSimTime + externalTotalDelaySeconds;
      deliveryTime = requestedDeliveryTime < transmissionEndTime
                        ? transmissionEndTime
                        : requestedDeliveryTime;
      status.SetTimeDelivery(deliveryTime);
   }
   if (!externalDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }

   // Determine if the recipient is actually able to receive a message based on simulation
'@
$guidedStatusNew = @'
   status.SetTimeLastXmtrStatusChange(aXmtr.GetLastStatusChangeTime());

   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   const std::string gateDropPrefix{"AFSIM_NS3_GATE_DROP:"};
   const std::string gateTotalDelayPrefix{"AFSIM_NS3_GATE_TOTAL_DELAY_SECONDS:"};
   const bool externalDrop =
      externalResult.mDetailedReason.compare(
         0, gateDropPrefix.size(), gateDropPrefix) == 0;
   bool externalTotalDelaySpecified = false;
   double externalTotalDelaySeconds = 0.0;
   if (externalResult.mDetailedReason.compare(
          0, gateTotalDelayPrefix.size(), gateTotalDelayPrefix) == 0)
   {
      try
      {
         externalTotalDelaySeconds = std::stod(
            externalResult.mDetailedReason.substr(gateTotalDelayPrefix.size()));
         if (externalTotalDelaySeconds < 0.0)
         {
            externalTotalDelaySeconds = 0.0;
         }
         externalTotalDelaySpecified = true;
      }
      catch (...)
      {
         externalTotalDelaySeconds = 0.0;
      }
   }
   if (externalDrop)
   {
      status.SetAbortDelivery(true);
   }
   else if (externalTotalDelaySpecified)
   {
      deliveryTime = aSimTime + externalTotalDelaySeconds;
      if (transmissionEndTime > deliveryTime)
      {
         transmissionEndTime = deliveryTime;
         status.SetTimeTransmissionEnd(transmissionEndTime);
      }
      status.SetTimeDelivery(deliveryTime);
   }
   if (!externalDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }

   // Determine if the recipient is actually able to receive a message based on simulation
'@
Update-SourceFile $guidedSource @(
    @{ Old = $guidedIncludeOld; New = $guidedIncludeNew },
    @{ Old = $guidedStringIncludeOld; New = $guidedStringIncludeNew },
    @{ Old = $transmittedOld; New = $transmittedNew },
    @{
        OldOptions = @($guidedStatusFloor, $guidedStatusCurrent, $guidedStatusPrevious, $guidedStatusOld)
        New = $guidedStatusNew
    }
)

$unguidedObserverOld = @'
   // Inform observers and listeners of the transmission attempt.
   if (aMessage.GetResult().mCheckedStatus != 0)
   {
      WsfObserver::MessageDeliveryAttempt(
         GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), aMessage.GetResult());
      WsfEM_Xmtr* xmtrPtr = aMessage.GetResult().GetTransmitter();
      if (xmtrPtr)
      {
         xmtrPtr->SetTransmissionEndTime(deliveryTime);
         xmtrPtr->NotifyListeners(aSimTime, aMessage.GetResult());
      }
   }
'@
$unguidedObserverPrevious = @'
   // Let the external ns-3 gate affect this message before AFSIM schedules delivery.
   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   if (externalResult.mExternalNetworkGateApplied)
   {
      if (externalResult.mExternalNetworkDrop)
      {
         status.SetAbortDelivery(true);
      }
      else if (externalResult.mExternalNetworkDelaySeconds > 0.0)
      {
         deliveryTime += externalResult.mExternalNetworkDelaySeconds;
         status.SetTimeDelivery(deliveryTime);
      }
   }
   if (!externalResult.mExternalNetworkGateApplied || !externalResult.mExternalNetworkDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }
   if (externalResult.mCheckedStatus != 0)
   {
      WsfEM_Xmtr* xmtrPtr = externalResult.GetTransmitter();
      if (xmtrPtr)
      {
         xmtrPtr->SetTransmissionEndTime(deliveryTime);
         xmtrPtr->NotifyListeners(aSimTime, externalResult);
      }
   }
'@
$unguidedStringIncludeOld = @'
#include "WsfEM_Xmtr.hpp"
'@
$unguidedStringIncludeNew = @'
#include "WsfEM_Xmtr.hpp"

#include <string>
'@
$unguidedObserverCurrent = @'
   // Let the external ns-3 gate affect this message before AFSIM schedules delivery.
   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   const std::string gateDropPrefix{"AFSIM_NS3_GATE_DROP:"};
   const std::string gateDelayPrefix{"AFSIM_NS3_GATE_DELAY_SECONDS:"};
   const bool externalDrop =
      externalResult.mDetailedReason.compare(
         0, gateDropPrefix.size(), gateDropPrefix) == 0;
   double externalDelaySeconds = 0.0;
   if (externalResult.mDetailedReason.compare(
          0, gateDelayPrefix.size(), gateDelayPrefix) == 0)
   {
      try
      {
         externalDelaySeconds = std::stod(
            externalResult.mDetailedReason.substr(gateDelayPrefix.size()));
      }
      catch (...)
      {
         externalDelaySeconds = 0.0;
      }
   }
   if (externalDrop)
   {
      status.SetAbortDelivery(true);
   }
   else if (externalDelaySeconds > 0.0)
   {
      deliveryTime += externalDelaySeconds;
      status.SetTimeDelivery(deliveryTime);
   }
   if (!externalDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }
   if (!externalDrop && externalResult.mCheckedStatus != 0)
   {
      WsfEM_Xmtr* xmtrPtr = externalResult.GetTransmitter();
      if (xmtrPtr)
      {
         xmtrPtr->SetTransmissionEndTime(deliveryTime);
         xmtrPtr->NotifyListeners(aSimTime, externalResult);
      }
   }
'@
$unguidedObserverFloor = @'
   // Let the external ns-3 gate affect this message before AFSIM schedules delivery.
   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   const std::string gateDropPrefix{"AFSIM_NS3_GATE_DROP:"};
   const std::string gateTotalDelayPrefix{"AFSIM_NS3_GATE_TOTAL_DELAY_SECONDS:"};
   const bool externalDrop =
      externalResult.mDetailedReason.compare(
         0, gateDropPrefix.size(), gateDropPrefix) == 0;
   bool externalTotalDelaySpecified = false;
   double externalTotalDelaySeconds = 0.0;
   if (externalResult.mDetailedReason.compare(
          0, gateTotalDelayPrefix.size(), gateTotalDelayPrefix) == 0)
   {
      try
      {
         externalTotalDelaySeconds = std::stod(
            externalResult.mDetailedReason.substr(gateTotalDelayPrefix.size()));
         if (externalTotalDelaySeconds < 0.0)
         {
            externalTotalDelaySeconds = 0.0;
         }
         externalTotalDelaySpecified = true;
      }
      catch (...)
      {
         externalTotalDelaySeconds = 0.0;
      }
   }
   if (externalDrop)
   {
      status.SetAbortDelivery(true);
   }
   else if (externalTotalDelaySpecified)
   {
      const double requestedDeliveryTime =
         aSimTime + externalTotalDelaySeconds;
      deliveryTime = requestedDeliveryTime < transmissionEndTime
                        ? transmissionEndTime
                        : requestedDeliveryTime;
      status.SetTimeDelivery(deliveryTime);
   }
   if (!externalDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }
   if (!externalDrop && externalResult.mCheckedStatus != 0)
   {
      WsfEM_Xmtr* xmtrPtr = externalResult.GetTransmitter();
      if (xmtrPtr)
      {
         xmtrPtr->SetTransmissionEndTime(deliveryTime);
         xmtrPtr->NotifyListeners(aSimTime, externalResult);
      }
   }
'@
$unguidedObserverNew = @'
   // Let the external ns-3 gate affect this message before AFSIM schedules delivery.
   auto& externalResult = aMessage.GetResult();
   WsfObserver::MessageDeliveryAttempt(
      GetSimulation())(aSimTime, &aXmtr, rcvrComm, *aMessage.SourceMessage(), externalResult);
   const std::string gateDropPrefix{"AFSIM_NS3_GATE_DROP:"};
   const std::string gateTotalDelayPrefix{"AFSIM_NS3_GATE_TOTAL_DELAY_SECONDS:"};
   const bool externalDrop =
      externalResult.mDetailedReason.compare(
         0, gateDropPrefix.size(), gateDropPrefix) == 0;
   bool externalTotalDelaySpecified = false;
   double externalTotalDelaySeconds = 0.0;
   if (externalResult.mDetailedReason.compare(
          0, gateTotalDelayPrefix.size(), gateTotalDelayPrefix) == 0)
   {
      try
      {
         externalTotalDelaySeconds = std::stod(
            externalResult.mDetailedReason.substr(gateTotalDelayPrefix.size()));
         if (externalTotalDelaySeconds < 0.0)
         {
            externalTotalDelaySeconds = 0.0;
         }
         externalTotalDelaySpecified = true;
      }
      catch (...)
      {
         externalTotalDelaySeconds = 0.0;
      }
   }
   if (externalDrop)
   {
      status.SetAbortDelivery(true);
   }
   else if (externalTotalDelaySpecified)
   {
      deliveryTime = aSimTime + externalTotalDelaySeconds;
      if (transmissionEndTime > deliveryTime)
      {
         transmissionEndTime = deliveryTime;
         status.SetTimeTransmissionEnd(transmissionEndTime);
      }
      status.SetTimeDelivery(deliveryTime);
   }
   if (!externalDrop)
   {
      WsfObserver::MessageTransmitted(GetSimulation())(
         aSimTime, &aXmtr, *(aMessage.SourceMessage()));
   }
   if (!externalDrop && externalResult.mCheckedStatus != 0)
   {
      WsfEM_Xmtr* xmtrPtr = externalResult.GetTransmitter();
      if (xmtrPtr)
      {
         xmtrPtr->SetTransmissionEndTime(deliveryTime);
         xmtrPtr->NotifyListeners(aSimTime, externalResult);
      }
   }
'@
Update-SourceFile $unguidedSource @(
    @{ Old = $unguidedStringIncludeOld; New = $unguidedStringIncludeNew },
    @{ Old = $transmittedOld; New = $transmittedNew },
    @{
        OldOptions = @($unguidedObserverFloor, $unguidedObserverCurrent, $unguidedObserverPrevious, $unguidedObserverOld)
        New = $unguidedObserverNew
    }
)

Write-Output "[done] AFSIM message delivery gate is ready."
